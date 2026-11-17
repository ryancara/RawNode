// Disposable native Windows research. This is not RawNode's persistence code.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::string_literals;
const std::string OLD = "OLD\0original photograph grade\r\nEND-OLD"s;
const std::string NEW = "NEW\0replacement grade with more bytes\r\nEND-NEW"s;
std::string current;
int failures = 0;

std::string quote(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { char b[7]; sprintf_s(b, "\\u%04x", unsigned(c)); out += b; }
    else out += char(c);
  }
  return out + '"';
}
std::string utf8(const std::wstring &s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  if (!n) throw std::runtime_error("UTF-16 conversion failed");
  std::string out(size_t(n), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n, nullptr, nullptr))
    throw std::runtime_error("UTF-16 conversion failed");
  return out;
}
std::wstring wide(const std::string &s) {
  if (s.empty() || s.find('\0') != std::string::npos) throw std::runtime_error("empty/NUL path");
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
  if (!n) throw std::runtime_error("invalid UTF-8 path");
  std::wstring out(size_t(n), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n))
    throw std::runtime_error("UTF-8 conversion failed");
  return out;
}
std::string hex(const unsigned char *p, size_t n) {
  static const char digits[] = "0123456789abcdef";
  std::string out; out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { out += digits[p[i] >> 4]; out += digits[p[i] & 15]; }
  return out;
}
std::string hex(const std::string &s) { return hex(reinterpret_cast<const unsigned char *>(s.data()), s.size()); }
struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE v = INVALID_HANDLE_VALUE): h(v) {}
  ~Handle() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
struct Result { bool ok; DWORD error; };
Result result(const char *operation, BOOL ok) {
  const DWORD error = ok ? 0 : GetLastError();
  std::cout << "{\"type\":\"operation\",\"case\":" << quote(current)
            << ",\"api\":" << quote(operation) << ",\"return\":" << (ok ? 1 : 0)
            << ",\"error\":" << error << "}\n";
  return {ok != FALSE, error};
}
void check(bool ok, const char *what) {
  if (!ok) ++failures;
  std::cout << "{\"type\":\"check\",\"case\":" << quote(current)
            << ",\"assertion\":" << quote(what) << ",\"pass\":" << (ok ? "true" : "false") << "}\n";
}
void require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
bool closeChecked(Handle &h) {
  HANDLE v = h.h; h.h = INVALID_HANDLE_VALUE;
  return result("CloseHandle", CloseHandle(v)).ok;
}
struct Digest {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  Digest() {
    require(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA provider");
    require(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "SHA hash");
  }
  ~Digest() { if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
  void add(const void *p, DWORD n) { require(BCryptHashData(hash, (PUCHAR)p, n, 0) >= 0, "SHA data"); }
  std::string finish() { unsigned char b[32]; require(BCryptFinishHash(hash, b, 32, 0) >= 0, "SHA finish"); return hex(b, 32); }
};
std::string sha(const std::string &s) { Digest d; d.add(s.data(), DWORD(s.size())); return d.finish(); }
uint64_t ticks(FILETIME v) { return (uint64_t(v.dwHighDateTime) << 32) | v.dwLowDateTime; }
struct State {
  bool exists = false;
  DWORD error = 0, attrs = 0, tag = 0, securityError = 0, readError = 0;
  uint64_t id = 0, volume = 0, created = 0, modified = 0, length = 0;
  std::string bytes, hash, security, dacl, reparse;
  bool complete = true;
};
std::string securityString(PSECURITY_DESCRIPTOR sd, SECURITY_INFORMATION flags) {
  LPWSTR value = nullptr;
  require(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, flags, &value, nullptr) != FALSE, "SDDL conversion");
  std::string out = utf8(value); LocalFree(value); return out;
}
State inspect(const fs::path &p) {
  State s;
  Handle h(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (h.h == INVALID_HANDLE_VALUE) { s.error = GetLastError(); return s; }
  s.exists = true;
  BY_HANDLE_FILE_INFORMATION info{};
  require(GetFileInformationByHandle(h.h, &info) != FALSE, "inspect identity");
  s.attrs = info.dwFileAttributes; s.id = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  s.volume = info.dwVolumeSerialNumber; s.created = ticks(info.ftCreationTime); s.modified = ticks(info.ftLastWriteTime);
  s.length = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  require(GetFileInformationByHandleEx(h.h, FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE, "inspect reparse tag");
  s.tag = tag.ReparseTag;
  PSECURITY_DESCRIPTOR sd = nullptr;
  s.securityError = GetSecurityInfo(h.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, nullptr, nullptr, &sd);
  if (!s.securityError) {
    s.security = securityString(sd, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION);
    s.dacl = securityString(sd, DACL_SECURITY_INFORMATION); LocalFree(sd);
  }
  if (s.attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
    unsigned char b[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]; DWORD n = 0;
    if (DeviceIoControl(h.h, FSCTL_GET_REPARSE_POINT, nullptr, 0, b, sizeof(b), &n, nullptr)) s.reparse = hex(b, n);
    else s.readError = GetLastError();
  } else if (!(s.attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    Handle data(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (data.h == INVALID_HANDLE_VALUE) s.readError = GetLastError();
    else {
      Digest digest; char b[65536]; DWORD n = 0;
      for (;;) {
        if (!ReadFile(data.h, b, sizeof(b), &n, nullptr)) { s.readError = GetLastError(); break; }
        if (!n) break;
        digest.add(b, n);
        if (s.bytes.size() + n <= 16384) s.bytes.append(b, n); else s.complete = false;
      }
      s.hash = digest.finish();
    }
  }
  return s;
}
void snapshot(const char *phase, const char *role, const fs::path &p, const State &s) {
  std::cout << "{\"type\":\"state\",\"case\":" << quote(current) << ",\"phase\":" << quote(phase)
    << ",\"role\":" << quote(role) << ",\"path\":" << quote(utf8(p.wstring())) << ",\"exists\":" << (s.exists ? "true" : "false")
    << ",\"open_error\":" << s.error << ",\"attributes\":" << s.attrs << ",\"reparse_tag\":" << s.tag
    << ",\"file_id\":" << s.id << ",\"volume_serial\":" << s.volume << ",\"creation_time\":" << s.created
    << ",\"last_write_time\":" << s.modified << ",\"length\":" << s.length << ",\"bytes_complete\":" << (s.complete ? "true" : "false")
    << ",\"bytes_hex\":" << quote(hex(s.bytes)) << ",\"sha256\":" << quote(s.hash)
    << ",\"security_sddl\":" << quote(s.security) << ",\"dacl_sddl\":" << quote(s.dacl)
    << ",\"security_error\":" << s.securityError << ",\"read_error\":" << s.readError
    << ",\"reparse_hex\":" << quote(s.reparse) << "}\n";
}
bool unchanged(const State &a, const State &b) {
  return a.exists && b.exists && a.id == b.id && a.volume == b.volume && a.attrs == b.attrs && a.tag == b.tag &&
    a.created == b.created && a.modified == b.modified && a.length == b.length && a.hash == b.hash &&
    a.security == b.security && !a.securityError && !b.securityError && !a.readError && !b.readError && a.reparse == b.reparse;
}
void make(const fs::path &p, const std::string &body, PSECURITY_DESCRIPTOR sd = nullptr) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  Handle h(CreateFileW(p.c_str(), GENERIC_WRITE, 0, sd ? &sa : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  require(result("CreateFileW CREATE_NEW", h.h != INVALID_HANDLE_VALUE).ok, "create fixture");
  DWORD written = 0;
  require(result("WriteFile", WriteFile(h.h, body.data(), DWORD(body.size()), &written, nullptr)).ok && written == body.size(), "write fixture");
  std::cout << "{\"type\":\"write_count\",\"case\":" << quote(current) << ",\"requested\":" << body.size() << ",\"written\":" << written << "}\n";
  require(result("FlushFileBuffers", FlushFileBuffers(h.h)).ok, "flush fixture");
  require(closeChecked(h), "close fixture");
}
struct Fixture {
  fs::path dir, d, t, b;
  State before, tempBefore, backupBefore;
  Fixture(const fs::path &root, const std::string &name, bool present = true) {
    current = name; dir = root / wide(name); fs::create_directory(dir);
    d = dir / L"photo.rawnode.json"; t = dir / L"sibling.tmp-unique"; b = root / L"backups" / (wide(name) + L".bin");
    if (present) make(d, OLD);
    make(t, NEW); make(b, OLD);
    check(inspect(b).bytes == OLD && (!present || inspect(b).id != inspect(d).id), "independent backup bytes and identity");
  }
  void capture() {
    before = inspect(d); tempBefore = inspect(t); backupBefore = inspect(b);
    snapshot("before", "destination", d, before); snapshot("before", "temporary", t, tempBefore); snapshot("before", "independent_backup", b, backupBefore);
    require(tempBefore.exists && !tempBefore.readError && !tempBefore.securityError, "temporary must be observable before publication");
    require(backupBefore.exists && !backupBefore.readError && !backupBefore.securityError, "backup must be observable before publication");
  }
  Result move(DWORD flags) {
    capture();
    std::cout << "{\"type\":\"move_parameters\",\"case\":" << quote(current) << ",\"flags\":" << flags << "}\n";
    auto r = result("MoveFileExW", MoveFileExW(t.c_str(), d.c_str(), flags));
    finish(r); return r;
  }
  void finish(Result r) {
    const auto after = inspect(d), tempAfter = inspect(t), backupAfter = inspect(b);
    snapshot("after", "destination", d, after); snapshot("after", "temporary", t, tempAfter); snapshot("after", "independent_backup", b, backupAfter);
    check(unchanged(backupBefore, backupAfter), "independent backup unchanged");
    if (!r.ok) {
      check(before.exists ? unchanged(before, after) : !after.exists && after.error == ERROR_FILE_NOT_FOUND, "failed publication preserved destination");
      check(unchanged(tempBefore, tempAfter), "failed publication preserved temporary");
    } else {
      check(after.exists && !after.readError && after.bytes == NEW, "successful publication has complete NEW bytes");
      check(!tempAfter.exists && tempAfter.error == ERROR_FILE_NOT_FOUND, "successful publication consumed temporary name");
      check(after.id == tempBefore.id && after.volume == tempBefore.volume, "published file has temporary identity");
    }
  }
};
std::wstring userSid() {
  Handle token; require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h) != FALSE, "open token");
  DWORD n = 0; GetTokenInformation(token.h, TokenUser, nullptr, 0, &n);
  std::vector<unsigned char> b(n); require(GetTokenInformation(token.h, TokenUser, b.data(), n, &n) != FALSE, "token user");
  LPWSTR text = nullptr; require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(b.data())->User.Sid, &text) != FALSE, "user SID");
  std::wstring out(text); LocalFree(text); return out;
}
void setDacl(const fs::path &p, const std::wstring &sddl, bool protect = true) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) != FALSE, "parse fixture ACL");
  PACL dacl = nullptr; BOOL present = FALSE, def = FALSE;
  require(GetSecurityDescriptorDacl(sd, &present, &dacl, &def) != FALSE && present, "fixture DACL");
  DWORD code = SetNamedSecurityInfoW(const_cast<LPWSTR>(p.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
                                    (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
                                    nullptr, nullptr, dacl, nullptr);
  LocalFree(sd);
  std::cout << "{\"type\":\"security_operation\",\"case\":" << quote(current) << ",\"api\":\"SetNamedSecurityInfoW\",\"error\":" << code << "}\n";
  require(!code, "set fixture DACL");
}
void copyDacl(const fs::path &source, const fs::path &temporary) {
  Handle from(CreateFileW(source.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  Handle to(CreateFileW(temporary.c_str(), WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  require(from.h != INVALID_HANDLE_VALUE && to.h != INVALID_HANDLE_VALUE, "ACL transfer handles");
  PACL dacl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
  DWORD code = GetSecurityInfo(from.h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
  require(!code, "get destination ACL");
  SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
  require(GetSecurityDescriptorControl(sd, &control, &revision) != FALSE, "ACL protection state");
  code = SetSecurityInfo(to.h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
    ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
    nullptr, nullptr, dacl, nullptr);
  LocalFree(sd);
  std::cout << "{\"type\":\"security_operation\",\"case\":" << quote(current) << ",\"api\":\"GetSecurityInfo/SetSecurityInfo DACL transfer\",\"error\":" << code << "}\n";
  require(!code, "apply destination ACL to temporary");
}
bool writable(const fs::path &p) {
  Handle h(CreateFileW(p.c_str(), FILE_WRITE_DATA | FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  return result("CreateFileW write permission preflight", h.h != INVALID_HANDLE_VALUE).ok;
}
bool regularDestination(const fs::path &p, bool ancestors) {
  fs::path at = p;
  do {
    DWORD attrs = GetFileAttributesW(at.c_str());
    const DWORD e = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    if (e && !(at == p && e == ERROR_FILE_NOT_FOUND)) return false;
    if (!e && ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) || (at == p && (attrs & FILE_ATTRIBUTE_DIRECTORY)))) return false;
    if (!ancestors || at == at.root_path()) break;
    at = at.parent_path();
  } while (!at.empty());
  return true;
}
int readerChild(int argc, wchar_t **argv) {
  if (argc != 7) return 10;
  Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]));
  Handle stop(OpenEventW(SYNCHRONIZE, FALSE, argv[5]));
  const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | (wcscmp(argv[3], L"delete") == 0 ? FILE_SHARE_DELETE : 0);
  Handle h(CreateFileW(argv[2], GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr));
  const DWORD e = h.h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  SetEvent(ready.h);
  if (e || WaitForSingleObject(stop.h, 30000) != WAIT_OBJECT_0) return 11;
  BY_HANDLE_FILE_INFORMATION info{}; GetFileInformationByHandle(h.h, &info);
  char b[1024]; DWORD n = 0; BOOL ok = ReadFile(h.h, b, sizeof(b), &n, nullptr); DWORD re = ok ? 0 : GetLastError();
  const std::string bytes(b, n);
  std::ofstream out{fs::path(argv[6])};
  out << "{\"type\":\"child_reader\",\"path\":" << quote(utf8(argv[2])) << ",\"share_delete\":" << ((share & FILE_SHARE_DELETE) ? "true" : "false")
      << ",\"open_error\":" << e << ",\"read_error\":" << re << ",\"bytes_hex\":" << quote(hex(bytes)) << ",\"sha256\":" << quote(sha(bytes))
      << ",\"file_id\":" << ((uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) << "}\n";
  out.close(); return ok && bytes == OLD && out.good() ? 0 : 12;
}
void otherProcess(Fixture &f, bool shareDelete, const fs::path &root) {
  std::wstring key = L"Local\\RawNodePersistence-" + std::to_wstring(GetCurrentProcessId()) + (shareDelete ? L"-yes" : L"-no");
  Handle ready(CreateEventW(nullptr, TRUE, FALSE, (key + L"-ready").c_str()));
  Handle stop(CreateEventW(nullptr, TRUE, FALSE, (key + L"-stop").c_str()));
  wchar_t exe[32768]; require(GetModuleFileNameW(nullptr, exe, 32768) != 0, "exe path");
  fs::path report = root / (shareDelete ? L"child-delete.json" : L"child-no-delete.json");
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --reader \"" + f.d.wstring() + L"\" " + (shareDelete ? L"delete" : L"keep") +
    L" \"" + key + L"-ready\" \"" + key + L"-stop\" \"" + report.wstring() + L"\"";
  STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
  require(result("CreateProcessW reader", CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)).ok, "start reader");
  Handle process(pi.hProcess), thread(pi.hThread);
  require(WaitForSingleObject(ready.h, 10000) == WAIT_OBJECT_0, "reader ready");
  DWORD exitCode = 0; GetExitCodeProcess(process.h, &exitCode); require(exitCode == STILL_ACTIVE, "reader open succeeded");
  f.move(MOVEFILE_REPLACE_EXISTING);
  SetEvent(stop.h); require(WaitForSingleObject(process.h, 10000) == WAIT_OBJECT_0, "reader exit");
  GetExitCodeProcess(process.h, &exitCode); check(exitCode == 0, "other-process reader retained OLD bytes");
  std::ifstream in(report); std::cout << in.rdbuf();
}
void junction(const fs::path &link, const fs::path &target) {
  fs::create_directory(link);
  struct Mount { DWORD tag; WORD length, reserved, so, sl, po, pl; wchar_t names[2048]; } data{};
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  const std::wstring sub = L"\\??\\" + fs::absolute(target).wstring(), print = fs::absolute(target).wstring();
  data.sl = WORD(sub.size() * 2); data.po = WORD((sub.size() + 1) * 2); data.pl = WORD(print.size() * 2);
  memcpy(data.names, sub.c_str(), data.sl + 2); memcpy(reinterpret_cast<char *>(data.names) + data.po, print.c_str(), data.pl + 2);
  data.length = WORD(8 + data.po + data.pl + 2);
  Handle h(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  require(h.h != INVALID_HANDLE_VALUE, "junction open"); DWORD n = 0;
  require(result("DeviceIoControl FSCTL_SET_REPARSE_POINT junction", DeviceIoControl(h.h, FSCTL_SET_REPARSE_POINT, &data,
    DWORD(8 + data.length), nullptr, 0, &n, nullptr)).ok, "junction create");
}
void environment(const fs::path &root) {
  wchar_t volume[32768], filesystem[128]; DWORD serial = 0, maxName = 0, flags = 0;
  require(GetVolumePathNameW(root.c_str(), volume, 32768) != FALSE, "volume path");
  require(GetVolumeInformationW(volume, nullptr, 0, &serial, &maxName, &flags, filesystem, 128) != FALSE, "volume information");
  std::cout << "{\"type\":\"environment\",\"root\":" << quote(utf8(root.wstring())) << ",\"volume\":" << quote(utf8(volume))
    << ",\"filesystem\":" << quote(utf8(filesystem)) << ",\"serial\":" << serial << ",\"filesystem_flags\":" << flags
    << ",\"drive_type\":" << GetDriveTypeW(volume) << ",\"user_sid\":" << quote(utf8(userSid())) << "}\n";
  require(wcscmp(filesystem, L"NTFS") == 0 && GetDriveTypeW(volume) == DRIVE_FIXED, "requires local fixed NTFS");
}
void diskFull(const fs::path &root, const fs::path &evidence) {
  current = "real_disk_full"; fs::create_directories(root); environment(root);
  const auto d = root / L"destination.json", t = root / L"sibling.tmp", b = evidence / L"disk-full-independent-backup.bin";
  make(d, OLD); make(b, OLD); const auto before = inspect(d), backup = inspect(b);
  snapshot("before", "destination", d, before); snapshot("before", "independent_backup", b, backup);
  Handle h(CreateFileW(t.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  require(result("CreateFileW CREATE_NEW", h.h != INVALID_HANDLE_VALUE).ok, "disk-full temporary");
  std::vector<char> bytes(65536, 'F'); uint64_t total = 0; DWORD code = 0;
  for (unsigned i = 0; i < 4096; ++i) {
    DWORD n = 0; BOOL ok = WriteFile(h.h, bytes.data(), DWORD(bytes.size()), &n, nullptr);
    if (!ok) { code = GetLastError(); break; } total += n;
  }
  std::cout << "{\"type\":\"disk_full_write\",\"case\":" << quote(current) << ",\"written\":" << total << ",\"return\":0,\"error\":" << code << "}\n";
  result("FlushFileBuffers after disk-full write", FlushFileBuffers(h.h)); closeChecked(h);
  snapshot("after", "destination", d, inspect(d)); snapshot("after", "partial_temporary", t, inspect(t)); snapshot("after", "independent_backup", b, inspect(b));
  check(code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL, "genuine disk-full write failure");
  check(unchanged(before, inspect(d)) && unchanged(backup, inspect(b)), "disk-full write preserved destination and independent backup");
  result("DeleteFileW partial temporary", DeleteFileW(t.c_str()));
  check(!inspect(t).exists && unchanged(before, inspect(d)), "disk-full ordinary cleanup preserved destination");
}
void experiments(const fs::path &root) {
  fs::create_directory(root / L"backups"); environment(root);
  { Fixture f(root, "create_new", false); check(f.move(0).ok, "new destination published"); }
  { Fixture f(root, "replace_existing"); check(f.move(MOVEFILE_REPLACE_EXISTING).ok, "existing destination replaced"); }
  { Fixture f(root, "no_replace_collision"); check(!f.move(0).ok, "creation refuses existing destination"); }
  for (unsigned i = 0; i < 12; ++i) { Fixture f(root, "repeat_" + std::to_string(i)); check(f.move(MOVEFILE_REPLACE_EXISTING).ok, "repeated replacement"); }
  { Fixture f(root, "unicode_names"); const std::string name = u8"caf\u00e9_\u65e5\u672c_\U0001f98a.rawnode.json";
    const auto dest = f.dir / wide(name); require(MoveFileExW(f.d.c_str(), dest.c_str(), 0) != FALSE, "unicode fixture rename"); f.d = dest;
    check(fs::u8path(utf8(f.d.wstring())).wstring() == f.d.wstring(), "C++17 u8path matches explicit UTF-8 conversion");
    check(f.move(MOVEFILE_REPLACE_EXISTING).ok, "Unicode destination replaced"); }
  { Fixture f(root, "unicode_create", false); f.d = f.dir / wide(u8"\u00e9_\u4e2d\U0001f680.json"); check(f.move(0).ok, "Unicode destination created"); }
  { current = "invalid_utf8"; bool rejected = false; try { wide(std::string("\xc0\xaf", 2)); } catch (...) { rejected = true; }
    check(rejected, "invalid UTF-8 rejected before filesystem operation"); }
  { Fixture f(root, "reader_without_delete"); Handle h(CreateFileW(f.d.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(h.h != INVALID_HANDLE_VALUE, "reader open"); check(!f.move(MOVEFILE_REPLACE_EXISTING).ok, "incompatible reader refuses replacement"); }
  { Fixture f(root, "reader_with_delete"); Handle h(CreateFileW(f.d.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    require(h.h != INVALID_HANDLE_VALUE, "reader open"); f.move(MOVEFILE_REPLACE_EXISTING);
    char b[1024]; DWORD n = 0; require(ReadFile(h.h, b, sizeof(b), &n, nullptr) != FALSE, "held reader read"); check(std::string(b, n) == OLD, "held reader retains complete OLD"); }
  { Fixture f(root, "other_process_without_delete"); otherProcess(f, false, root); }
  { Fixture f(root, "other_process_with_delete"); otherProcess(f, true, root); }
  { Fixture f(root, "readonly_destination"); require(SetFileAttributesW(f.d.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set read-only");
    check(!f.move(MOVEFILE_REPLACE_EXISTING).ok, "read-only destination refuses replacement"); }
  { Fixture f(root, "write_denied_raw_rename"); setDacl(f.d, L"D:P(D;;0x6;;;" + userSid() + L")(A;;FA;;;" + userSid() + L")(A;;FA;;;SY)");
    check(!writable(f.d), "destination genuinely denies writing"); f.move(MOVEFILE_REPLACE_EXISTING); }
  { Fixture f(root, "write_denied_guard"); setDacl(f.d, L"D:P(D;;0x6;;;" + userSid() + L")(A;;FA;;;" + userSid() + L")(A;;FA;;;SY)");
    f.capture(); check(!writable(f.d), "guard refuses write-denied destination"); f.finish({false, ERROR_ACCESS_DENIED}); }
  { Fixture f(root, "delete_denied"); setDacl(f.d, L"D:P(D;;SD;;;" + userSid() + L")(A;;FA;;;" + userSid() + L")(A;;FA;;;SY)");
    // Protect the source from the deliberately non-inheritable directory ACL.
    setDacl(f.t, L"D:P(A;;FA;;;" + userSid() + L")(A;;FA;;;SY)");
    setDacl(f.dir, L"D:P(D;;DC;;;" + userSid() + L")(A;;FA;;;" + userSid() + L")(A;;FA;;;SY)");
    check(!f.move(MOVEFILE_REPLACE_EXISTING).ok, "genuine ACL denial refuses rename"); }
  { Fixture f(root, "directory_destination", false); fs::create_directory(f.d); check(!f.move(MOVEFILE_REPLACE_EXISTING).ok, "directory refuses replacement"); }
  { Fixture f(root, "missing_parent", false); f.d = f.dir / L"missing" / L"destination.json";
    f.capture(); const auto r = result("MoveFileExW missing parent", MoveFileExW(f.t.c_str(), f.d.c_str(), 0));
    const auto after = inspect(f.d); snapshot("after", "destination", f.d, after); snapshot("after", "temporary", f.t, inspect(f.t));
    snapshot("after", "independent_backup", f.b, inspect(f.b));
    check(!r.ok && after.error == ERROR_PATH_NOT_FOUND && unchanged(f.tempBefore, inspect(f.t)), "missing parent fails without implicit creation"); }
  { Fixture f(root, "temporary_collision"); f.capture(); Handle h(CreateFileW(f.t.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
    check(!result("CreateFileW CREATE_NEW collision", h.h != INVALID_HANDLE_VALUE).ok, "CREATE_NEW refuses collision"); f.finish({false, ERROR_FILE_EXISTS}); }
  { Fixture f(root, "failed_write_and_flush"); f.capture(); Handle h(CreateFileW(f.t.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    require(h.h != INVALID_HANDLE_VALUE, "read-only temporary handle"); DWORD n = 0;
    check(!result("WriteFile read-only handle", WriteFile(h.h, NEW.data(), DWORD(NEW.size()), &n, nullptr)).ok, "genuine write access failure");
    check(!result("FlushFileBuffers read-only handle", FlushFileBuffers(h.h)).ok, "genuine flush access failure"); closeChecked(h); f.finish({false, ERROR_ACCESS_DENIED}); }
  { current = "double_handle_close"; Handle h(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    require(h.h != nullptr, "close test event"); const HANDLE closed = h.h; require(closeChecked(h), "first event close");
    check(!result("CloseHandle already closed real handle", CloseHandle(closed)).ok, "already-closed handle reports failure"); }
  { Fixture f(root, "partial_write_abort"); f.capture(); const auto partial = f.dir / L"partial.tmp"; make(partial, NEW.substr(0, NEW.size()/2));
    snapshot("before_cleanup", "partial_temporary", partial, inspect(partial)); result("DeleteFileW partial temporary", DeleteFileW(partial.c_str()));
    check(!inspect(partial).exists, "injected pre-publication abort cleans partial temporary"); f.finish({false, ERROR_WRITE_FAULT}); }
  const std::wstring specialAcl = L"D:P(A;;FA;;;" + userSid() + L")(A;;FR;;;BU)(A;;FA;;;SY)";
  { Fixture f(root, "explicit_dacl_raw_rename"); setDacl(f.d, specialAcl); f.move(MOVEFILE_REPLACE_EXISTING);
    check(inspect(f.d).dacl == f.tempBefore.dacl, "raw rename publishes temporary DACL");
    std::cout << "{\"type\":\"finding\",\"case\":" << quote(current) << ",\"destination_dacl_preserved\":" << (inspect(f.d).dacl == f.before.dacl ? "true" : "false") << "}\n"; }
  { Fixture f(root, "preserved_explicit_dacl"); setDacl(f.d, specialAcl); const auto original = inspect(f.d); copyDacl(f.d, f.t);
    check(f.move(MOVEFILE_REPLACE_EXISTING).ok && inspect(f.d).dacl == original.dacl, "explicit protected destination DACL copied before publication"); }
  { Fixture f(root, "inherited_dacl"); const auto original = inspect(f.d);
    check(f.move(MOVEFILE_REPLACE_EXISTING).ok && inspect(f.d).dacl == original.dacl, "same-directory default inheritance retained in this fixture"); }
  { Fixture f(root, "preserved_unprotected_dacl"); setDacl(f.d, L"D:(A;;FR;;;WD)", false);
    const auto original = inspect(f.d); copyDacl(f.d, f.t);
    check(f.move(MOVEFILE_REPLACE_EXISTING).ok && inspect(f.d).dacl == original.dacl, "unprotected explicit and inherited DACL copied before publication"); }
  { Fixture f(root, "private_creation_acl", false); require(DeleteFileW(f.t.c_str()) != FALSE, "discard own default-ACL fixture temporary");
    PSECURITY_DESCRIPTOR sd = nullptr;
    const std::wstring acl = L"D:P(A;;FA;;;" + userSid() + L")(A;;FA;;;SY)";
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &sd, nullptr) != FALSE, "private creation ACL");
    make(f.t, NEW, sd); LocalFree(sd); const auto original = inspect(f.t);
    check(f.move(0).ok && inspect(f.d).dacl == original.dacl && original.dacl.find("D:P") == 0, "private DACL established at CREATE_NEW and retained on publication"); }
  { Fixture f(root, "hidden_archive_attributes"); require(SetFileAttributesW(f.d.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE) != FALSE, "set hidden archive");
    f.move(MOVEFILE_REPLACE_EXISTING); }
  { Fixture f(root, "preserved_hidden_attributes"); require(SetFileAttributesW(f.d.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE) != FALSE, "destination attrs");
    require(SetFileAttributesW(f.t.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE) != FALSE, "temporary attrs");
    check(f.move(MOVEFILE_REPLACE_EXISTING).ok && inspect(f.d).attrs == f.before.attrs, "supported attributes copied before publication"); }
  { Fixture f(root, "symlink_refusal", false); const auto target = f.dir / L"linked-target.json"; make(target, OLD); const auto original = inspect(target);
    auto r = result("CreateSymbolicLinkW", CreateSymbolicLinkW(f.d.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
    if (!r.ok) r = result("CreateSymbolicLinkW privileged fallback for fixture only", CreateSymbolicLinkW(f.d.c_str(), target.c_str(), 0));
    if (r.ok) { f.capture(); check(!regularDestination(f.d, true), "preflight refuses destination symlink"); f.finish({false, ERROR_REPARSE_TAG_INVALID});
      snapshot("after", "symlink_target", target, inspect(target)); check(unchanged(original, inspect(target)), "symlink target preserved"); }
    else std::cout << "{\"type\":\"skip\",\"case\":" << quote(current) << ",\"reason\":\"symlink privilege unavailable\"}\n";
  }
  { Fixture f(root, "junction_parent_refusal", false); const auto target = f.dir / L"target", link = f.dir / L"junction";
    fs::create_directory(target); const auto actual = target / L"destination.json"; make(actual, OLD); junction(link, target); f.d = link / L"destination.json";
    f.capture(); snapshot("before", "junction", link, inspect(link)); check(regularDestination(f.d, false), "final-component-only preflight misses parent junction");
    check(!regularDestination(f.d, true), "ancestor preflight refuses parent junction"); f.finish({false, ERROR_REPARSE_TAG_INVALID});
    snapshot("after", "junction", link, inspect(link)); snapshot("after", "junction_target_file", actual, inspect(actual));
    require(RemoveDirectoryW(link.c_str()) != FALSE, "remove disposable junction"); }
  { Fixture f(root, "source_sharing_conflict"); Handle h(CreateFileW(f.t.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    require(h.h != INVALID_HANDLE_VALUE, "source reader"); check(!f.move(MOVEFILE_REPLACE_EXISTING).ok, "source sharing conflict refuses rename");
    check(!result("DeleteFileW locked temporary cleanup", DeleteFileW(f.t.c_str())).ok, "cleanup refusal leaves locked temporary for diagnostic");
    check(unchanged(f.before, inspect(f.d)) && unchanged(f.tempBefore, inspect(f.t)), "blocked cleanup preserved destination and exact remaining temporary"); }
  { Fixture f(root, "rename_failure_cleanup"); Handle h(CreateFileW(f.d.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    require(h.h != INVALID_HANDLE_VALUE, "destination reader"); check(!f.move(MOVEFILE_REPLACE_EXISTING).ok, "genuine rename conflict");
    check(result("DeleteFileW own temporary after failed rename", DeleteFileW(f.t.c_str())).ok, "ordinary failed rename cleanup succeeds");
    check(!inspect(f.t).exists && unchanged(f.before, inspect(f.d)) && unchanged(f.backupBefore, inspect(f.b)), "cleanup only removed own temporary"); }
  { Fixture f(root, "handle_rename"); Handle h(CreateFileW(f.t.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(h.h != INVALID_HANDLE_VALUE, "rename source handle");
    Handle denied(CreateFileW(f.t.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(!result("CreateFileW DELETE reopen while source handle excludes delete sharing", denied.h != INVALID_HANDLE_VALUE).ok, "held source identity cannot be reopened for path-based rename");
    f.capture(); const std::wstring target = f.d.wstring();
    std::vector<unsigned char> b(offsetof(FILE_RENAME_INFO, FileName) + (target.size()+1)*sizeof(wchar_t));
    auto *info = reinterpret_cast<FILE_RENAME_INFO *>(b.data()); info->ReplaceIfExists = TRUE; info->RootDirectory = nullptr;
    info->FileNameLength = DWORD(target.size()*sizeof(wchar_t)); memcpy(info->FileName, target.c_str(), (target.size()+1)*sizeof(wchar_t));
    auto r = result("SetFileInformationByHandle FileRenameInfo", SetFileInformationByHandle(h.h, FileRenameInfo, info, DWORD(b.size())));
    closeChecked(h); f.finish(r); }
  { current = "atomic_visibility"; const auto dir = root / L"atomic_visibility"; fs::create_directory(dir);
    const auto d = dir / L"destination.json", t = dir / L"sibling.tmp", backup = root / L"backups" / L"atomic.bin";
    const std::string a = "BEGIN-A\n" + std::string(4080, 'A') + "\nEND-A", b = "BEGIN-B\n" + std::string(4080, 'B') + "\nEND-B";
    make(d, a); make(backup, a); snapshot("before", "destination", d, inspect(d)); snapshot("before", "independent_backup", backup, inspect(backup));
    const auto originalBackup = inspect(backup); std::atomic<bool> done{false}; uint64_t oldReads=0, newReads=0, invalid=0, missing=0, sharing=0, other=0;
    std::thread observer([&] { do {
      Handle h(CreateFileW(d.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
      if (h.h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); if(e==ERROR_FILE_NOT_FOUND) ++missing; else if(e==ERROR_SHARING_VIOLATION) ++sharing; else ++other; continue; }
      char data[8192]; DWORD n=0; if(!ReadFile(h.h, data, sizeof(data), &n, nullptr)) { ++other; continue; }
      const std::string value(data,n); if(value==a) ++oldReads; else if(value==b) ++newReads; else ++invalid;
    } while(!done.load()); });
    unsigned succeeded=0, failed=0; for(unsigned i=0;i<200;++i) {
      make(t, i%2 ? a : b); const auto beforeMove=inspect(d), beforeTemp=inspect(t);
      BOOL ok=MoveFileExW(t.c_str(),d.c_str(),MOVEFILE_REPLACE_EXISTING); DWORD e=ok?0:GetLastError();
      std::cout << "{\"type\":\"visibility_move\",\"case\":" << quote(current) << ",\"iteration\":" << i << ",\"return\":" << (ok?1:0) << ",\"error\":" << e << "}\n";
      if(ok) ++succeeded; else { ++failed; const auto afterMove=inspect(d), afterTemp=inspect(t);
        snapshot("before_failed_move", "destination", d, beforeMove); snapshot("after_failed_move", "destination", d, afterMove);
        snapshot("after_failed_move", "temporary", t, afterTemp);
        check(unchanged(beforeMove,afterMove) && unchanged(beforeTemp,afterTemp), "failed stress rename preserved destination and temporary");
        DeleteFileW(t.c_str()); }
    }
    done.store(true); observer.join();
    std::cout << "{\"type\":\"visibility\",\"case\":" << quote(current) << ",\"replacements\":" << succeeded << ",\"failed_replacements\":" << failed
      << ",\"old_reads\":" << oldReads << ",\"new_reads\":" << newReads << ",\"invalid_payloads\":" << invalid << ",\"missing\":" << missing << ",\"sharing\":" << sharing << ",\"other_errors\":" << other << "}\n";
    snapshot("after", "destination", d, inspect(d)); snapshot("after", "temporary", t, inspect(t)); snapshot("after", "independent_backup", backup, inspect(backup));
    check(oldReads && newReads && !invalid && !missing && !other, "observer read only complete old/new payloads and no missing destination");
    check(unchanged(originalBackup, inspect(backup)), "visibility backup unchanged");
  }
}
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc > 1 && wcscmp(argv[1], L"--reader") == 0) return readerChild(argc, argv);
    if (argc == 4 && wcscmp(argv[1], L"--disk-full") == 0) diskFull(fs::absolute(argv[2]), fs::absolute(argv[3]));
    else { require(argc == 2, "usage: probe.exe fresh-root"); const auto root=fs::absolute(argv[1]);
      require(!fs::exists(root), "research root must not exist"); fs::create_directories(root); experiments(root); }
    std::cout << "{\"type\":\"summary\",\"failed_checks\":" << failures << "}\n";
    return failures ? 1 : 0;
  } catch (const std::exception &e) {
    std::cout << "{\"type\":\"fatal\",\"case\":" << quote(current) << ",\"message\":" << quote(e.what()) << ",\"last_error\":" << GetLastError() << "}\n";
    return 2;
  }
}
