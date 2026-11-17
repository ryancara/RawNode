// Research-only native probe comparing three Windows replacement strategies on
// disposable fixtures:
//   bare    - sibling + MoveFileExW only (no checks, no metadata transfer)
//   pr      - PR #117 production writer at 5e5aa22 (persist_detail::replaceWindowsFile)
//   minimal - research/windows-minimal-persistence/MinimalWindowsFile.cpp
// Phases (see workflow): user-prep, admin-prep, user-run, admin-efs, fs, diskfull, admin-inspect.
// Every observation is printed as one JSON line; results are interpreted in the report.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>

#include "MinimalWindowsFile.h"
#ifndef RESEARCH_NO_PR
#include "persist/ProjectPersistPriv.h"
#include "persist/WindowsFile.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

namespace fs = std::filesystem;
namespace {
constexpr DWORD shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
const wchar_t *foreignSid = L"S-1-5-21-1000000001-1000000002-1000000003-1001";

// ---------- small utilities ----------
std::string utf8(const std::wstring &text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(size > 0 ? size : 0), '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}
std::string esc(const std::string &s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += char(c);
  }
  return o;
}
std::string q(const std::string &s) { return "\"" + esc(s) + "\""; }
std::string hex(DWORD v) { char b[16]; std::snprintf(b, sizeof b, "0x%lx", (unsigned long)v); return b; }

void writeRaw(const std::wstring &path, const std::string &bytes, DWORD disposition = CREATE_ALWAYS) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture write " + utf8(path) + " error " + std::to_string(GetLastError()));
  DWORD n = 0;
  const BOOL ok = WriteFile(h, bytes.data(), DWORD(bytes.size()), &n, nullptr);
  CloseHandle(h);
  if (!ok || n != bytes.size()) throw std::runtime_error("fixture write failed " + utf8(path));
}
std::string readAll(const std::wstring &path, DWORD &error) {
  error = 0;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, shareAll, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) { error = GetLastError(); return {}; }
  std::string out;
  char buffer[65536];
  DWORD n = 0;
  while (ReadFile(h, buffer, sizeof buffer, &n, nullptr) && n) out.append(buffer, n);
  CloseHandle(h);
  return out;
}
std::string sidText(PSID sid) {
  if (!sid) return "null";
  wchar_t *text = nullptr;
  if (!ConvertSidToStringSidW(sid, &text)) return "error";
  std::string out = utf8(text);
  LocalFree(text);
  return out;
}
std::string sddl(PSECURITY_DESCRIPTOR sd, SECURITY_INFORMATION what) {
  wchar_t *text = nullptr;
  if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, what, &text, nullptr))
    return "error " + std::to_string(GetLastError());
  std::string out = utf8(text);
  LocalFree(text);
  return out;
}
std::wstring currentUserSid() {
  HANDLE token = nullptr;
  OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD n = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &n);
  std::vector<unsigned char> data(n);
  GetTokenInformation(token, TokenUser, data.data(), n, &n);
  CloseHandle(token);
  wchar_t *text = nullptr;
  ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &text);
  std::wstring out(text);
  LocalFree(text);
  return out;
}
bool enablePrivilege(const wchar_t *name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
  TOKEN_PRIVILEGES p{};
  p.PrivilegeCount = 1;
  p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const bool ok = LookupPrivilegeValueW(nullptr, name, &p.Privileges[0].Luid) &&
                  AdjustTokenPrivileges(token, FALSE, &p, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return ok;
}
DWORD setSecurity(const std::wstring &path, const std::wstring &text, SECURITY_INFORMATION what) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &sd, nullptr)) return GetLastError();
  PSID owner = nullptr, group = nullptr;
  PACL dacl = nullptr, sacl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  GetSecurityDescriptorOwner(sd, &owner, &defaulted);
  GetSecurityDescriptorGroup(sd, &group, &defaulted);
  GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
  GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted);
  const DWORD code = SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT, what, owner, group, dacl, sacl);
  LocalFree(sd);
  return code;
}
void must(DWORD code, const char *what) {
  if (code != ERROR_SUCCESS) throw std::runtime_error(std::string(what) + " error " + std::to_string(code));
}
void addStream(const std::wstring &file, const std::wstring &stream, const std::string &bytes) {
  writeRaw(file + L":" + stream, bytes);
}
bool compress(const std::wstring &path, bool directory) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, shareAll, nullptr, OPEN_EXISTING,
                         directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  USHORT format = COMPRESSION_FORMAT_DEFAULT;
  DWORD n = 0;
  const BOOL ok = DeviceIoControl(h, FSCTL_SET_COMPRESSION, &format, sizeof format, nullptr, 0, &n, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}
void junction(const std::wstring &link, const std::wstring &target) {
  CreateDirectoryW(link.c_str(), nullptr);
  struct Mount { DWORD tag; WORD size, reserved, so, sl, po, pl; wchar_t names[2048]; } data{};
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  const std::wstring sub = L"\\??\\" + target, print = target;
  data.sl = WORD(sub.size() * 2);
  data.po = WORD((sub.size() + 1) * 2);
  data.pl = WORD(print.size() * 2);
  std::memcpy(data.names, sub.c_str(), data.sl + 2);
  std::memcpy(reinterpret_cast<char *>(data.names) + data.po, print.c_str(), data.pl + 2);
  data.size = WORD(8 + data.po + data.pl + 2);
  HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  DWORD n = 0;
  const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &data, 8 + data.size, nullptr, 0, &n, nullptr);
  CloseHandle(h);
  if (!ok) throw std::runtime_error("junction " + utf8(link) + " error " + std::to_string(GetLastError()));
}

// ---------- independent observation ----------
struct State {
  bool exists = false;
  DWORD openError = 0, readError = 0, attributes = 0, links = 0;
  unsigned long long id = 0;
  std::string content, owner, group, dacl, label, sacl, streams;
};
State observe(const std::wstring &path, bool withSacl = false) {
  State s;
  HANDLE h = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES | (withSacl ? ACCESS_SYSTEM_SECURITY : 0),
                         shareAll, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) { s.openError = GetLastError(); return s; }
  s.exists = true;
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileInformationByHandle(h, &info)) {
    s.attributes = info.dwFileAttributes;
    s.links = info.nNumberOfLinks;
    s.id = (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  }
  SECURITY_INFORMATION what = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                              LABEL_SECURITY_INFORMATION | (withSacl ? SACL_SECURITY_INFORMATION : 0);
  PSECURITY_DESCRIPTOR sd = nullptr;
  PSID owner = nullptr, group = nullptr;
  const DWORD code = GetSecurityInfo(h, SE_FILE_OBJECT, what, &owner, &group, nullptr, nullptr, &sd);
  if (code == ERROR_SUCCESS) {
    s.owner = sidText(owner);
    s.group = sidText(group);
    s.dacl = sddl(sd, DACL_SECURITY_INFORMATION);
    s.label = sddl(sd, LABEL_SECURITY_INFORMATION);
    if (withSacl) s.sacl = sddl(sd, SACL_SECURITY_INFORMATION);
    LocalFree(sd);
  } else {
    s.dacl = "error " + std::to_string(code);
  }
  CloseHandle(h);
  if (!(s.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) s.content = readAll(path, s.readError);
  WIN32_FIND_STREAM_DATA stream{};
  HANDLE find = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &stream, 0);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (!s.streams.empty()) s.streams += ",";
      s.streams += utf8(stream.cStreamName);
    } while (FindNextStreamW(find, &stream));
    FindClose(find);
  }
  return s;
}
std::string json(const State &s) {
  std::ostringstream o;
  o << "{\"exists\":" << (s.exists ? "true" : "false");
  if (!s.exists) { o << ",\"openError\":" << s.openError << "}"; return o.str(); }
  o << ",\"content\":" << q(s.content.size() > 120 ? s.content.substr(0, 40) + "...(" + std::to_string(s.content.size()) + " bytes)" : s.content)
    << ",\"readError\":" << s.readError << ",\"attributes\":" << q(hex(s.attributes)) << ",\"links\":" << s.links
    << ",\"id\":" << q(std::to_string(s.id)) << ",\"owner\":" << q(s.owner) << ",\"group\":" << q(s.group)
    << ",\"dacl\":" << q(s.dacl) << ",\"label\":" << q(s.label) << ",\"streams\":" << q(s.streams);
  if (!s.sacl.empty()) o << ",\"sacl\":" << q(s.sacl);
  o << "}";
  return o.str();
}
bool sameMetadata(const State &a, const State &b) {
  return a.exists == b.exists && a.content == b.content && a.attributes == b.attributes && a.links == b.links &&
         a.id == b.id && a.owner == b.owner && a.group == b.group && a.dacl == b.dacl && a.label == b.label &&
         a.streams == b.streams;
}
int leftovers(const std::wstring &dir) {
  int count = 0;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().wstring();
    if (name.rfind(L".rawnode-tmp-", 0) == 0 || name.rfind(L".bare-tmp-", 0) == 0) ++count;
  }
  return count;
}
void removeLeftovers(const std::wstring &dir) {
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().wstring();
    if (name.rfind(L".rawnode-tmp-", 0) == 0 || name.rfind(L".bare-tmp-", 0) == 0) DeleteFileW(entry.path().c_str());
  }
}

// ---------- the three strategies ----------
enum class Variant { Bare, Pr, Minimal, MinimalAnyFs };
const char *name(Variant v) {
  switch (v) {
    case Variant::Bare: return "bare";
    case Variant::Pr: return "pr";
    case Variant::Minimal: return "minimal";
    case Variant::MinimalAnyFs: return "minimal-anyfs";
  }
  return "?";
}
bool bareReplace(const std::wstring &path, const std::string &body, std::string &error) {
  const std::wstring temp = (fs::path(path).parent_path() / (L".bare-tmp-" + std::to_wstring(GetTickCount64()))).wstring();
  HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { error = "bare create error " + std::to_string(GetLastError()); return false; }
  DWORD n = 0;
  const BOOL wrote = WriteFile(h, body.data(), DWORD(body.size()), &n, nullptr);
  const BOOL closed = CloseHandle(h);
  if (!wrote || !closed || n != body.size()) { error = "bare write error"; DeleteFileW(temp.c_str()); return false; }
  const bool present = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (!MoveFileExW(temp.c_str(), path.c_str(), present ? MOVEFILE_REPLACE_EXISTING : 0)) {
    error = "bare MoveFileExW error " + std::to_string(GetLastError());
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}
bool write(Variant v, const std::wstring &path, const std::string &body, std::string &error,
           minimal::Fault fault = minimal::Fault::None) {
  error.clear();
  switch (v) {
    case Variant::Bare: return bareReplace(path, body, error);
    case Variant::Pr:
#ifndef RESEARCH_NO_PR
      return persist_detail::replaceWindowsFile(utf8(path), body, error, persist_detail::FileFault::None);
#else
      error = "pr unavailable in this build";
      return false;
#endif
    case Variant::Minimal: return minimal::replaceFile(utf8(path), body, error, {true, fault});
    case Variant::MinimalAnyFs: return minimal::replaceFile(utf8(path), body, error, {false, fault});
  }
  return false;
}
const std::vector<Variant> allVariants = {Variant::Bare, Variant::Pr, Variant::Minimal};

void report(const std::string &phase, const std::string &caseName, const std::string &variant, bool ok,
            const std::string &error, const State &before, const State &after, int left,
            const std::string &extra = "") {
  std::printf("{\"phase\":%s,\"case\":%s,\"variant\":%s,\"ok\":%s,\"error\":%s,\"unchanged\":%s,\"leftovers\":%d,"
              "\"before\":%s,\"after\":%s%s%s}\n",
              q(phase).c_str(), q(caseName).c_str(), q(variant).c_str(), ok ? "true" : "false", q(error).c_str(),
              sameMetadata(before, after) ? "true" : "false", left, json(before).c_str(), json(after).c_str(),
              extra.empty() ? "" : ",", extra.c_str());
  std::fflush(stdout);
}

// ---------- fixture cases on local NTFS ----------
// Each case gets root\<case>\<variant>\v.json. user-prep runs as the ordinary
// user; admin-prep adds what needs privileges; user-run saves as the user.
struct Case {
  const char *name;
  bool newFile;
  std::function<void(const std::wstring &dir, const std::wstring &file, const std::wstring &user)> userPrep;
  std::function<void(const std::wstring &dir, const std::wstring &file, const std::wstring &user)> adminPrep;
  std::wstring writePath = L"v.json";  // relative to the variant directory
  std::wstring observeExtra;           // second file observed after the save
};
std::vector<Case> cases() {
  const auto plain = [](const std::wstring &, const std::wstring &f, const std::wstring &) { writeRaw(f, "OLD"); };
  const auto attrs = [](DWORD a) {
    return [a](const std::wstring &, const std::wstring &f, const std::wstring &) {
      writeRaw(f, "OLD");
      if (!SetFileAttributesW(f.c_str(), a)) throw std::runtime_error("attributes");
    };
  };
  const auto dacl = [](const std::wstring &text, bool protect) {
    return [text, protect](const std::wstring &, const std::wstring &f, const std::wstring &user) {
      writeRaw(f, "OLD");
      std::wstring t = text;
      for (size_t at; (at = t.find(L"{U}")) != std::wstring::npos;) t.replace(at, 3, user);
      must(setSecurity(f, t, DACL_SECURITY_INFORMATION |
                                 (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION)),
           "dacl fixture");
    };
  };
  std::vector<Case> c;
  c.push_back({"new-file", true, nullptr, nullptr});
  c.push_back({"replace-plain", false, plain, nullptr});
  c.push_back({"dacl-protected-private", false, dacl(L"D:P(A;;FA;;;{U})(A;;FA;;;SY)", true), nullptr});
  c.push_back({"dacl-explicit-deny-network", false, dacl(L"D:(D;;FA;;;NU)", false), nullptr});
  c.push_back({"dacl-explicit-allow-guests", false, dacl(L"D:(A;;FR;;;BG)", false), nullptr});
  c.push_back({"deny-write-data", false, dacl(L"D:P(D;;0x6;;;{U})(A;;FA;;;{U})(A;;FA;;;SY)", true), nullptr});
  c.push_back({"readonly", false, attrs(FILE_ATTRIBUTE_READONLY), nullptr});
  c.push_back({"owner-administrators", false, plain,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 must(setSecurity(f, L"O:BAG:BA", OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION), "owner BA");
               }});
  c.push_back({"owner-foreign-sid", false, plain,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 must(setSecurity(f, std::wstring(L"O:") + foreignSid + L"G:" + foreignSid,
                                  OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION), "owner foreign");
               }});
  c.push_back({"label-low", false, plain,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 must(setSecurity(f, L"S:(ML;;NW;;;LW)", LABEL_SECURITY_INFORMATION), "label low");
               }});
  c.push_back({"label-high", false, plain,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 must(setSecurity(f, L"S:(ML;;NW;;;HI)", LABEL_SECURITY_INFORMATION), "label high");
               }});
  c.push_back({"audit-explicit", false, plain,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 must(setSecurity(f, L"S:(AU;SAFA;FA;;;WD)", SACL_SECURITY_INFORMATION), "audit explicit");
               }});
  // Inherited auditing: the folder audits; the user creates then saves twice.
  c.push_back({"audit-inherited-folder", true, nullptr,
               [](const std::wstring &d, const std::wstring &, const std::wstring &) {
                 must(setSecurity(d, L"S:(AU;OICISA;FA;;;WD)", SACL_SECURITY_INFORMATION), "audit folder");
               }});
  c.push_back({"ads-zone-identifier", false,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 writeRaw(f, "OLD");
                 addStream(f, L"Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://example.invalid/\r\n");
               }, nullptr});
  c.push_back({"ads-dropbox-style", false,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 writeRaw(f, "OLD");
                 addStream(f, L"com.dropbox.attributes", std::string("x\x9c\x01\x02\x03 binary-attrs", 18));
               }, nullptr});
  c.push_back({"ads-arbitrary-data", false,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 writeRaw(f, "OLD");
                 addStream(f, L"notes", "user data in a stream");
               }, nullptr});
  c.push_back({"attr-archive-cleared", false, attrs(FILE_ATTRIBUTE_NORMAL), nullptr});
  c.push_back({"attr-hidden", false, attrs(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE), nullptr});
  c.push_back({"attr-not-indexed", false, attrs(FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_ARCHIVE), nullptr});
  c.push_back({"attr-system", false, attrs(FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE), nullptr});
  c.push_back({"attr-hidden-system", false, attrs(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE), nullptr});
  c.push_back({"compressed-file", false,
               [](const std::wstring &, const std::wstring &f, const std::wstring &) {
                 writeRaw(f, "OLD");
                 if (!compress(f, false)) throw std::runtime_error("compress file");
               }, nullptr});
  c.push_back({"compressed-folder", false,
               [](const std::wstring &d, const std::wstring &f, const std::wstring &) {
                 if (!compress(d, true)) throw std::runtime_error("compress folder");
                 writeRaw(f, "OLD");
               }, nullptr});
  {
    Case hard{"hard-link", false,
              [](const std::wstring &d, const std::wstring &f, const std::wstring &) {
                writeRaw(f, "OLD");
                if (!CreateHardLinkW((d + L"\\alias.json").c_str(), f.c_str(), nullptr)) throw std::runtime_error("hard link");
              }, nullptr};
    hard.observeExtra = L"alias.json";
    c.push_back(hard);
  }
  {
    Case jn{"junction-parent-ntfs", false,
            [](const std::wstring &d, const std::wstring &, const std::wstring &) {
              CreateDirectoryW((d + L"\\real").c_str(), nullptr);
              writeRaw(d + L"\\real\\v.json", "OLD");
              junction(d + L"\\link", d + L"\\real");
            }, nullptr};
    jn.writePath = L"link\\v.json";
    jn.observeExtra = L"real\\v.json";
    c.push_back(jn);
  }
  {
    Case sym{"symlink-final", false,
             [](const std::wstring &d, const std::wstring &, const std::wstring &) { writeRaw(d + L"\\target.json", "TARGET"); },
             [](const std::wstring &d, const std::wstring &f, const std::wstring &) {
               if (!CreateSymbolicLinkW(f.c_str(), (d + L"\\target.json").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
                 throw std::runtime_error("symlink " + std::to_string(GetLastError()));
             }};
    sym.observeExtra = L"target.json";
    c.push_back(sym);
  }
  // Genuine temporary-creation failure: the user may write the file but not add files.
  c.push_back({"folder-denies-create", false, plain,
               [](const std::wstring &d, const std::wstring &f, const std::wstring &user) {
                 must(setSecurity(f, L"D:P(A;;FA;;;" + user + L")(A;;FA;;;SY)(A;;FA;;;BA)",
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION), "file dacl");
                 must(setSecurity(d, L"D:P(A;;0x1200a9;;;" + user + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION), "folder dacl");
               }});
  return c;
}
std::wstring variantDir(const std::wstring &root, const char *caseName, Variant v) {
  return root + L"\\" + fs::path(caseName).wstring() + L"\\" + fs::path(name(v)).wstring();
}

void userPrep(const std::wstring &root) {
  const std::wstring user = currentUserSid();
  for (const auto &c : cases())
    for (Variant v : allVariants) {
      const auto dir = variantDir(root, c.name, v);
      fs::create_directories(dir);
      if (c.userPrep) c.userPrep(dir, dir + L"\\v.json", user);
    }
  std::printf("{\"phase\":\"user-prep\",\"user\":%s}\n", q(utf8(user)).c_str());
}
void adminPrep(const std::wstring &root, const std::wstring &user) {
  const bool security = enablePrivilege(L"SeSecurityPrivilege"), restore = enablePrivilege(L"SeRestorePrivilege");
  std::printf("{\"phase\":\"admin-prep\",\"seSecurity\":%s,\"seRestore\":%s}\n", security ? "true" : "false",
              restore ? "true" : "false");
  for (const auto &c : cases())
    for (Variant v : allVariants) {
      const auto dir = variantDir(root, c.name, v);
      try {
        if (c.adminPrep) c.adminPrep(dir, dir + L"\\v.json", user);
      } catch (const std::exception &e) {
        std::printf("{\"phase\":\"admin-prep\",\"case\":%s,\"variant\":%s,\"prepError\":%s}\n", q(c.name).c_str(),
                    q(name(v)).c_str(), q(e.what()).c_str());
      }
    }
  std::fflush(stdout);
}

void runCases(const std::wstring &root) {
  for (const auto &c : cases())
    for (Variant v : allVariants) {
      const auto dir = variantDir(root, c.name, v);
      const auto file = dir + L"\\" + c.writePath;
      const State before = observe(file);
      const State extraBefore = c.observeExtra.empty() ? State{} : observe(dir + L"\\" + c.observeExtra);
      std::string error;
      const std::string body = std::string("NEW:") + c.name + ":" + name(v);
      const bool ok = write(v, file, body, error);
      const State after = observe(file);
      std::string extra;
      if (!c.observeExtra.empty())
        extra = "\"extraBefore\":" + json(extraBefore) + ",\"extraAfter\":" + json(observe(dir + L"\\" + c.observeExtra));
      if (c.newFile) {
        // Compare with an ordinary file created in the same folder with default security.
        const auto control = dir + L"\\control.json";
        writeRaw(control, "control");
        extra += std::string(extra.empty() ? "" : ",") + "\"control\":" + json(observe(control));
        DeleteFileW(control.c_str());
      }
      report("user-run", c.name, name(v), ok, error, before, after, leftovers(dir), extra);
      removeLeftovers(dir);
      if (c.newFile && ok) {  // A second save of the file the strategy just created.
        const State second = observe(file);
        const bool again = write(v, file, body + ":second", error);
        report("user-run", std::string(c.name) + "-second-save", name(v), again, error, second, observe(file), leftovers(dir));
        removeLeftovers(dir);
      }
    }
}

// Real sharing conflicts: an independent reader holds the destination open.
void readerCases(const std::wstring &root, const char *label, Variant v) {
  const auto dir = root + L"\\readers-" + fs::path(name(v)).wstring();
  fs::create_directories(dir);
  const auto file = dir + L"\\v.json";
  for (DWORD share : {DWORD(FILE_SHARE_READ | FILE_SHARE_WRITE), shareAll}) {
    writeRaw(file, "OLD");
    HANDLE reader = CreateFileW(file.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
    const State before = observe(file);
    std::string error;
    const bool ok = write(v, file, "NEW", error);
    char held[4] = {};
    DWORD n = 0;
    ReadFile(reader, held, 3, &n, nullptr);
    CloseHandle(reader);
    const State after = observe(file);
    report(label, share == shareAll ? "reader-share-delete" : "reader-no-delete-share", name(v), ok, error, before,
           after, leftovers(dir), "\"heldReaderRead\":" + q(std::string(held, n)));
    removeLeftovers(dir);
    std::string retryError;
    const bool retry = write(v, file, "RETRY", retryError);
    std::printf("{\"phase\":%s,\"case\":\"reader-retry-after-close\",\"variant\":%s,\"ok\":%s,\"error\":%s}\n", q(label).c_str(),
                q(name(v)).c_str(), retry ? "true" : "false", q(retryError).c_str());
  }
}

// Injected failures exercise every abort path of the minimal writer.
void faultCases(const std::wstring &root, const char *label, Variant v) {
  const auto dir = root + L"\\faults-" + fs::path(name(v)).wstring();
  fs::create_directories(dir);
  const auto file = dir + L"\\v.json";
  const std::pair<minimal::Fault, const char *> faults[] = {
      {minimal::Fault::Write, "write"}, {minimal::Fault::Flush, "flush"}, {minimal::Fault::Close, "close"},
      {minimal::Fault::Replace, "replace"}, {minimal::Fault::Cleanup, "cleanup"}};
  for (bool present : {true, false})
    for (const auto &fault : faults) {
      DeleteFileW(file.c_str());
      if (present) writeRaw(file, "OLD");
      const State before = observe(file);
      std::string error;
      const bool ok = write(v, file, std::string(4096, 'N'), error, fault.first);
      report(label, std::string("fault-") + fault.second + (present ? "-existing" : "-absent"), name(v), ok, error,
             before, observe(file), leftovers(dir));
      removeLeftovers(dir);
    }
}

void unicodeCases(const std::wstring &root) {
  const auto dir = root + L"\\unicode-writer";
  fs::create_directories(dir);
  for (const wchar_t *stem : {L"café", L"日本語", L"\U0001F98A-fox"}) {
    const std::wstring file = dir + L"\\" + stem + L".nef.rawnode.json";
    for (int round = 0; round < 2; ++round) {
      const State before = observe(file);
      std::string error;
      const bool ok = minimal::replaceFile(utf8(file), "NEW" + std::to_string(round), error);
      report("user-run", "unicode-" + utf8(stem) + (round ? "-replace" : "-create"), "minimal", ok, error, before,
             observe(file), leftovers(dir));
    }
  }
}

// Share mode 0 already keeps the in-progress sibling private.
void shareZeroCheck(const std::wstring &root) {
  const auto file = root + L"\\share-zero.tmp";
  HANDLE owner = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE other = CreateFileW(file.c_str(), GENERIC_READ, shareAll, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD code = other == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  if (other != INVALID_HANDLE_VALUE) CloseHandle(other);
  CloseHandle(owner);
  DeleteFileW(file.c_str());
  std::printf("{\"phase\":\"user-run\",\"case\":\"share-zero-second-open\",\"openError\":%lu}\n", (unsigned long)code);
}

// ---------- EFS (runner account, which has a loaded profile) ----------
void efsCases(const std::wstring &root) {
  for (bool folderEncrypted : {false, true})
    for (Variant v : allVariants) {
      const auto dir = root + (folderEncrypted ? L"\\efs-folder\\" : L"\\efs-file\\") + fs::path(name(v)).wstring();
      fs::create_directories(dir);
      const auto file = dir + L"\\v.json";
      bool prepared = true;
      if (folderEncrypted) prepared = EncryptFileW(dir.c_str()) != FALSE;
      writeRaw(file, "OLD");
      if (!folderEncrypted) prepared = EncryptFileW(file.c_str()) != FALSE;
      const DWORD prepError = prepared ? 0 : GetLastError();
      const State before = observe(file);
      std::string error;
      const bool ok = write(v, file, "NEW", error);
      report("admin-efs", folderEncrypted ? "efs-encrypted-folder" : "efs-encrypted-file", name(v), ok, error, before,
             observe(file), leftovers(dir), "\"prepError\":" + std::to_string(prepError));
      removeLeftovers(dir);
    }
}

// ---------- filesystem matrix (FAT32, exFAT, NTFS, SMB, junction to FAT32) ----------
std::string payload(char tag) {
  std::string s(4094, tag);
  s.insert(0, "<");
  s.push_back('>');
  return s;
}
void visibility(const std::wstring &dir, const char *label, int iterations) {
  const auto file = dir + L"\\visible.json";
  const std::string a = payload('A'), b = payload('B');
  std::string error;
  minimal::replaceFile(utf8(file), a, error, {false});
  std::atomic<bool> stop{false};
  std::map<std::string, int> reads;
  std::thread reader([&] {
    std::vector<char> buffer(16384);
    while (!stop) {
      HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, shareAll, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h == INVALID_HANDLE_VALUE) { ++reads["open-error-" + std::to_string(GetLastError())]; continue; }
      DWORD n = 0, total = 0;
      while (ReadFile(h, buffer.data() + total, DWORD(buffer.size() - total), &n, nullptr) && n) total += n;
      CloseHandle(h);
      const std::string got(buffer.data(), total);
      ++reads[got == a ? "old-or-a" : got == b ? "new-or-b" : "partial-or-unexpected"];
    }
  });
  std::map<std::string, int> writes;
  for (int i = 0; i < iterations; ++i) {
    const bool ok = minimal::replaceFile(utf8(file), (i % 2) ? a : b, error, {false});
    if (ok) ++writes["ok"];
    else {
      const auto at = error.find("Windows error ");
      ++writes[at == std::string::npos ? error : "error-" + error.substr(at + 14, error.find(')', at) - at - 14)];
    }
  }
  stop = true;
  reader.join();
  std::ostringstream o;
  o << "{\"phase\":" << q(label) << ",\"case\":\"concurrent-visibility\",\"variant\":\"minimal-anyfs\",\"writes\":{";
  bool first = true;
  for (const auto &w : writes) { o << (first ? "" : ",") << q(w.first) << ":" << w.second; first = false; }
  o << "},\"reads\":{";
  first = true;
  for (const auto &r : reads) { o << (first ? "" : ",") << q(r.first) << ":" << r.second; first = false; }
  o << "},\"leftovers\":" << leftovers(dir) << ",\"final\":" << json(observe(file)) << "}";
  std::puts(o.str().c_str());
  std::fflush(stdout);
}
void filesystemMatrix(const std::wstring &dir, const char *label) {
  fs::create_directories(dir);
  wchar_t fsName[MAX_PATH + 1] = {};
  DWORD flags = 0;
  GetVolumeInformationW((fs::path(dir).root_path().wstring()).c_str(), nullptr, 0, nullptr, nullptr, &flags, fsName, MAX_PATH + 1);
  std::printf("{\"phase\":%s,\"case\":\"volume\",\"rootFilesystem\":%s,\"rootFlags\":%s}\n", q(label).c_str(),
              q(utf8(fsName)).c_str(), q(hex(flags)).c_str());
  for (Variant v : {Variant::Bare, Variant::Pr, Variant::Minimal, Variant::MinimalAnyFs}) {
    const auto sub = dir + L"\\" + fs::path(name(v)).wstring();
    fs::create_directories(sub);
    const auto file = sub + L"\\v.json";
    for (const char *step : {"create", "replace"}) {
      const State before = observe(file);
      std::string error;
      const bool ok = write(v, file, std::string("NEW-") + step, error);
      report(label, step, name(v), ok, error, before, observe(file), leftovers(sub));
      removeLeftovers(sub);
    }
  }
  for (Variant v : {Variant::Bare, Variant::MinimalAnyFs}) {
    const auto sub = dir + L"\\attrs-" + fs::path(name(v)).wstring();
    fs::create_directories(sub);
    const auto file = sub + L"\\v.json";
    for (DWORD attributes : {DWORD(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE), DWORD(FILE_ATTRIBUTE_NORMAL),
                             DWORD(FILE_ATTRIBUTE_READONLY)}) {
      writeRaw(file, "OLD");
      SetFileAttributesW(file.c_str(), attributes);
      const State before = observe(file);
      std::string error;
      const bool ok = write(v, file, "NEW", error);
      report(label, "attributes-" + hex(attributes), name(v), ok, error, before, observe(file), leftovers(sub));
      removeLeftovers(sub);
      SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
  }
  {
    const auto sub = dir + L"\\dacl-minimal-anyfs";
    fs::create_directories(sub);
    const auto file = sub + L"\\v.json";
    writeRaw(file, "OLD");
    const DWORD set = setSecurity(file, L"D:P(A;;FA;;;" + currentUserSid() + L")(A;;FA;;;SY)",
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION);
    const State before = observe(file);
    std::string error;
    const bool ok = write(Variant::MinimalAnyFs, file, "NEW", error);
    report(label, "dacl-protected", "minimal-anyfs", ok, error, before, observe(file), leftovers(sub),
           "\"fixtureDaclError\":" + std::to_string(set));
  }
  readerCases(dir, label, Variant::MinimalAnyFs);
  faultCases(dir, label, Variant::MinimalAnyFs);
  visibility(dir, label, 200);
}

// Genuine storage exhaustion while writing the sibling.
void diskFull(const std::wstring &dir, const char *label) {
  fs::create_directories(dir);
  const auto file = dir + L"\\v.json", filler = dir + L"\\filler.bin";
  writeRaw(file, "OLD");
  ULARGE_INTEGER freeBytes{}, total{}, totalFree{};
  GetDiskFreeSpaceExW(dir.c_str(), &freeBytes, &total, &totalFree);
  HANDLE h = CreateFileW(filler.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  LARGE_INTEGER size{};
  size.QuadPart = LONGLONG(freeBytes.QuadPart) - 256 * 1024;
  const bool filled = SetFilePointerEx(h, size, nullptr, FILE_BEGIN) && SetEndOfFile(h);
  CloseHandle(h);
  GetDiskFreeSpaceExW(dir.c_str(), &freeBytes, &total, &totalFree);
  const State before = observe(file);
  std::string error;
  const bool ok = minimal::replaceFile(utf8(file), std::string(4 << 20, 'F'), error, {false});
  report(label, "disk-full", "minimal-anyfs", ok, error, before, observe(file), leftovers(dir),
         "\"filled\":" + std::string(filled ? "true" : "false") + ",\"freeAfterFill\":" + std::to_string(freeBytes.QuadPart));
  removeLeftovers(dir);
  DeleteFileW(filler.c_str());
}

void inspect(const std::wstring &root) {
  const bool security = enablePrivilege(L"SeSecurityPrivilege");
  std::printf("{\"phase\":\"admin-inspect\",\"seSecurity\":%s}\n", security ? "true" : "false");
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto path = it->path().wstring();
    const bool directory = it->is_directory(ec) && !it->is_symlink(ec);
    if (directory && path.find(L"audit-inherited") == std::wstring::npos) continue;
    std::printf("{\"phase\":\"admin-inspect\",\"path\":%s,\"state\":%s}\n", q(utf8(path.substr(root.size()))).c_str(),
                json(observe(path, true)).c_str());
  }
  std::fflush(stdout);
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    if (argc < 3) throw std::runtime_error("usage: probe <phase> <root> [extra]");
    const std::wstring phase = argv[1], root = argv[2];
    std::printf("{\"phase\":\"start\",\"mode\":%s,\"user\":%s,\"acp\":%u}\n", q(utf8(phase)).c_str(),
                q(utf8(currentUserSid())).c_str(), GetACP());
    if (phase == L"user-prep") userPrep(root);
    else if (phase == L"admin-prep") adminPrep(root, argc > 3 ? argv[3] : L"");
    else if (phase == L"user-run") {
      runCases(root);
      readerCases(root, "user-run", Variant::Minimal);
      readerCases(root, "user-run", Variant::Bare);
      faultCases(root, "user-run", Variant::Minimal);
      unicodeCases(root);
      shareZeroCheck(root);
    } else if (phase == L"admin-efs") efsCases(root);
    else if (phase == L"fs") filesystemMatrix(root, argc > 3 ? utf8(argv[3]).c_str() : "fs");
    else if (phase == L"diskfull") diskFull(root, argc > 3 ? utf8(argv[3]).c_str() : "diskfull");
    else if (phase == L"admin-inspect") inspect(root);
    else throw std::runtime_error("unknown phase");
    std::printf("{\"phase\":\"done\",\"mode\":%s}\n", q(utf8(phase)).c_str());
    return 0;
  } catch (const std::exception &e) {
    std::printf("{\"phase\":\"fatal\",\"error\":%s}\n", q(e.what()).c_str());
    return 2;
  }
}
