// Observation helpers mechanically reused from approved research 20f820ec.
// Not an experimental writer or production replacement.
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
  DWORD error = 0, attrs = 0, tag = 0, tagError = 0, securityError = 0, readError = 0;
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
  if (!GetFileInformationByHandleEx(h.h, FileAttributeTagInfo, &tag, sizeof(tag))) {
    s.tagError = GetLastError();
    require(!(s.attrs & FILE_ATTRIBUTE_REPARSE_POINT), "cannot inspect actual reparse point");
  }
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
    << ",\"tag_error\":" << s.tagError << ",\"file_id\":" << s.id << ",\"volume_serial\":" << s.volume << ",\"creation_time\":" << s.created
    << ",\"last_write_time\":" << s.modified << ",\"length\":" << s.length << ",\"bytes_complete\":" << (s.complete ? "true" : "false")
    << ",\"bytes_hex\":" << quote(hex(s.bytes)) << ",\"sha256\":" << quote(s.hash)
    << ",\"security_sddl\":" << quote(s.security) << ",\"dacl_sddl\":" << quote(s.dacl)
    << ",\"security_error\":" << s.securityError << ",\"read_error\":" << s.readError
    << ",\"reparse_hex\":" << quote(s.reparse) << "}\n";
}
bool unchanged(const State &a, const State &b) {
  return a.exists && b.exists && a.id == b.id && a.volume == b.volume && a.attrs == b.attrs && a.tag == b.tag &&
    a.created == b.created && a.modified == b.modified && a.length == b.length && a.hash == b.hash &&
    a.security == b.security && a.securityError == b.securityError && a.tagError == b.tagError && !a.readError && !b.readError && a.reparse == b.reparse;
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
