#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>

#include "persist/WindowsFile.h"
#include "persist/ProjectPersistPriv.h"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
namespace {
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  bool close() {
    const HANDLE owned = value;
    value = INVALID_HANDLE_VALUE;
    return CloseHandle(owned) != FALSE;
  }
};
struct FreeLocal { void operator()(void *p) const { if (p) LocalFree(p); } };
using LocalMemory = std::unique_ptr<void, FreeLocal>;

std::string nativeError(const char *operation, DWORD code) {
  wchar_t *message = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
  LocalMemory memory(message);
  std::string text, ignored;
  if (message) persist_detail::windowsUtf8(message, text, ignored);
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
  return std::string(operation) + " (Windows error " + std::to_string(code) + ")" +
      (text.empty() ? "" : ": " + text);
}
bool fail(std::string &error, const char *operation, DWORD code) {
  error = nativeError(operation, code);
  return false;
}
constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD openFlags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
constexpr DWORD ordinaryAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;

bool attributes(HANDLE file, FILE_ATTRIBUTE_TAG_INFO &info, std::string &error) {
  if (GetFileType(file) != FILE_TYPE_DISK) { error = "Destination is not a regular disk file"; return false; }
  if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)))
    return fail(error, "Cannot inspect file attributes", GetLastError());
  return true;
}

struct Security {
  LocalMemory memory;
  PSID owner = nullptr, group = nullptr;
  PACL dacl = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control = 0;
};
bool readSecurity(HANDLE file, Security &out, std::string &error) {
  // These SACL subsets are readable with READ_CONTROL. Do not discard integrity,
  // resource, central-access, trust or access-filter protections on replacement.
  constexpr SECURITY_INFORMATION fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
      LABEL_SECURITY_INFORMATION | ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION |
      PROCESS_TRUST_LABEL_SECURITY_INFORMATION | ACCESS_FILTER_SECURITY_INFORMATION;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD code = GetSecurityInfo(file, SE_FILE_OBJECT, fields, &out.owner, &out.group, &out.dacl, nullptr, &descriptor);
  out.memory.reset(descriptor);
  if (code) return fail(error, "Cannot inspect destination security", code);
  DWORD revision = 0;
  if (!GetSecurityDescriptorControl(descriptor, &out.control, &revision))
    return fail(error, "Cannot inspect inheritance protection", GetLastError());
  PACL sacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
  if (!GetSecurityDescriptorSacl(descriptor, &present, &sacl, &defaulted))
    return fail(error, "Cannot inspect access protections", GetLastError());
  // An empty filtered ACL can still hide auditing ACEs. Refuse non-null system
  // ACLs, not just the enforcement ACEs visible to an unprivileged query.
  constexpr SECURITY_DESCRIPTOR_CONTROL specialControl = SE_SACL_PROTECTED | SE_DACL_UNTRUSTED |
      SE_SERVER_SECURITY | SE_RM_CONTROL_VALID;
  if ((present && sacl) || (out.control & specialControl)) {
    error = "Unsupported integrity, resource, central-access or other system protection";
    return false;
  }
  if (!out.dacl || !out.owner || !out.group) {
    error = "Unsupported absent security owner, group or DACL";
    return false;
  }
  for (DWORD i = 0; i < out.dacl->AceCount; ++i) {
    void *ace = nullptr;
    if (!GetAce(out.dacl, i, &ace)) return fail(error, "Cannot inspect destination DACL", GetLastError());
    const auto type = static_cast<ACE_HEADER *>(ace)->AceType;
    if (type != ACCESS_ALLOWED_ACE_TYPE && type != ACCESS_DENIED_ACE_TYPE) {
      error = "Unsupported object, callback or other special DACL entry";
      return false;
    }
  }
  return true;
}
bool defaultStreamOnly(const fs::path &path, std::string &error) {
  WIN32_FIND_STREAM_DATA stream{};
  const HANDLE search = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &stream, 0);
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    return code == ERROR_HANDLE_EOF || fail(error, "Cannot inspect alternate streams", code);
  }
  bool ordinary = true;
  do { if (std::wcscmp(stream.cStreamName, L"::$DATA") != 0) ordinary = false; }
  while (FindNextStreamW(search, &stream));
  const DWORD code = GetLastError();
  FindClose(search);
  if (code != ERROR_HANDLE_EOF) return fail(error, "Cannot enumerate alternate streams", code);
  if (!ordinary) error = "Destination has unsupported alternate streams";
  return ordinary;
}
bool privateSecurity(LocalMemory &descriptor, std::string &error) {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
    return fail(error, "Cannot read current user", GetLastError());
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> data(size);
  if (!size || !GetTokenInformation(token.value, TokenUser, data.data(), size, &size))
    return fail(error, "Cannot read current user SID", GetLastError());
  wchar_t *sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid))
    return fail(error, "Cannot encode current user SID", GetLastError());
  LocalMemory sidMemory(sid);
  const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")(A;;FA;;;SY)";
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr))
    return fail(error, "Cannot create private temporary permissions", GetLastError());
  descriptor.reset(raw);
  return true;
}

struct Temporary {
  Handle file;
  fs::path path;
  ~Temporary() {
    if (file.value != INVALID_HANDLE_VALUE) file.close();
    if (!path.empty()) DeleteFileW(path.c_str());
  }
  void cleanup(std::string &error, persist_detail::FileFault fault) {
    if (file.value != INVALID_HANDLE_VALUE && !file.close())
      error += "; " + nativeError("Cannot close temporary file during cleanup", GetLastError());
    const BOOL removed = fault == persist_detail::FileFault::Cleanup ? FALSE : DeleteFileW(path.c_str());
    const DWORD code = fault == persist_detail::FileFault::Cleanup ? ERROR_ACCESS_DENIED : GetLastError();
    if (!removed) error += "; cannot remove temporary file " + path.u8string() + ": " + nativeError("DeleteFileW", code);
    // A reported cleanup failure leaves its exact path for the caller. Do not
    // silently retry in the destructor or remove another temporary-looking file.
    path.clear();
  }
};
}  // namespace

bool persist_detail::windowsUtf8(const wchar_t *text, std::string &out, std::string &error) {
  if (!text || !*text) { out.clear(); return true; }
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
  if (!length) { error = "Invalid Unicode path"; return false; }
  std::vector<char> bytes(static_cast<size_t>(length));
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, bytes.data(), length, nullptr, nullptr)) {
    error = "Cannot convert Unicode path to UTF-8"; return false;
  }
  out.assign(bytes.data(), bytes.size() - 1);
  return true;
}
bool persist_detail::windowsPath(const std::string &text, fs::path &path, std::string &error) {
  if (text.empty() || text.find('\0') != std::string::npos || text.size() >= MAX_PATH * 4) {
    error = "Empty, embedded-NUL or unsupported long persistence path"; return false;
  }
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
  if (!length) return fail(error, "Invalid UTF-8 persistence path", GetLastError());
  std::vector<wchar_t> wide(static_cast<size_t>(length));
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, wide.data(), length))
    return fail(error, "Cannot convert UTF-8 path", GetLastError());
  wchar_t absolute[MAX_PATH];
  const DWORD count = GetFullPathNameW(wide.data(), MAX_PATH, absolute, nullptr);
  if (!count || count >= MAX_PATH) { error = "Unsupported long or invalid persistence path"; return false; }
  path = absolute;
  const std::wstring name = path.wstring();
  if (name.size() < 4 || name[1] != L':' || name[2] != L'\\' || name.find(L':', 2) != std::wstring::npos ||
      name[0] == L'\\' || std::wstring(wide.data()).find(L"\\\\") == 0) {
    error = "Only conventional local disk paths are supported; device, network and stream paths are refused"; return false;
  }
  for (const auto &part : path.relative_path()) {
    const auto component = part.wstring();
    if (component.empty() || component.back() == L'.' || component.back() == L' ') {
      error = "Persistence path has an ambiguous trailing dot or space"; return false;
    }
  }
  return true;
}
bool persist_detail::windowsParents(const fs::path &path, std::string &error, bool allowMissing) {
  const fs::path root = path.root_path();
  if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
    error = "Atomic persistence supports local fixed NTFS volumes only"; return false;
  }
  fs::path parent = root;
  std::vector<fs::path> parents{root};
  for (const auto &part : path.parent_path().relative_path()) { parent /= part; parents.push_back(parent); }
  for (const auto &directory : parents) {
    Handle h(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING, openFlags, nullptr));
    if (h.value == INVALID_HANDLE_VALUE) {
      const DWORD code = GetLastError();
      // Configuration-directory creation may extend an already-qualified
      // ancestor. File reads/writes require every parent to exist.
      if (allowMissing && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) return true;
      return fail(error, "Cannot inspect destination parent", code);
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!attributes(h.value, info, error)) return false;
    constexpr DWORD allowed = ordinaryAttributes | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM;
    if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & ~allowed)) {
      error = "Unsupported parent: junction, symlink, cloud/reparse point or special directory attributes"; return false;
    }
    wchar_t filesystem[64];
    if (!GetVolumeInformationByHandleW(h.value, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 64))
      return fail(error, "Cannot qualify parent filesystem", GetLastError());
    if (std::wcscmp(filesystem, L"NTFS") != 0) { error = "Atomic persistence supports local NTFS only"; return false; }
  }
  return true;
}

bool persist_detail::replaceWindowsFile(const std::string &text, const std::string &body, std::string &error, FileFault fault) {
  using Fault = FileFault;
  error.clear();
  fs::path path;
  const auto refuse = [&] { error = text + ": " + error; return false; };
  if (!windowsPath(text, path, error) || !windowsParents(path, error)) return refuse();
  if (fault == Fault::PermissionStat) { fail(error, "Cannot read destination permissions", ERROR_READ_FAULT); return refuse(); }
  Handle destination(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, sharing, nullptr, OPEN_EXISTING, openFlags, nullptr));
  const DWORD opened = destination.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  if (opened && opened != ERROR_FILE_NOT_FOUND) { fail(error, "Cannot inspect destination", opened); return refuse(); }
  const bool present = !opened;
  FILE_ATTRIBUTE_TAG_INFO info{};
  Security security;
  if (present) {
    if (!attributes(destination.value, info, error)) return refuse();
    if (info.FileAttributes & FILE_ATTRIBUTE_READONLY) { error = "Destination is not writable (read-only)"; return refuse(); }
    if (info.FileAttributes & ~(ordinaryAttributes | FILE_ATTRIBUTE_NORMAL)) {
      error = "Destination is not a supported regular file: reparse, cloud or special attributes"; return refuse();
    }
    // Native NTFS publication sets ARCHIVE even if it was cleared on the
    // completed sibling. Do not silently change an existing backup marker or
    // add a fallible metadata mutation after publication.
    if (!(info.FileAttributes & FILE_ATTRIBUTE_ARCHIVE)) {
      error = "Unsupported destination with ARCHIVE cleared: Windows replacement cannot preserve that attribute"; return refuse();
    }
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandle(destination.value, &identity)) { fail(error, "Cannot inspect file identity", GetLastError()); return refuse(); }
    if (identity.nNumberOfLinks != 1) { error = "Unsupported multiply-linked destination"; return refuse(); }
    if (!readSecurity(destination.value, security, error) || !defaultStreamOnly(path, error)) return refuse();
    Handle writable(CreateFileW(path.c_str(), FILE_WRITE_DATA | FILE_APPEND_DATA, sharing, nullptr, OPEN_EXISTING, openFlags, nullptr));
    if (fault == Fault::PermissionCheck || writable.value == INVALID_HANDLE_VALUE) {
      fail(error, "Destination is not writable", fault == Fault::PermissionCheck ? ERROR_ACCESS_DENIED : GetLastError()); return refuse();
    }
  }
  LocalMemory privateDescriptor;
  if (!privateSecurity(privateDescriptor, error)) return refuse();
  Temporary temp;
  SECURITY_ATTRIBUTES sa{sizeof(sa), privateDescriptor.get(), FALSE};
  for (int attempt = 0; attempt < 16; ++attempt) {
    unsigned char random[16];
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
      error = "Cannot generate sibling temporary name"; return refuse();
    }
    std::wstring name = L".rawnode-tmp-";
    for (unsigned char byte : random) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
    const fs::path candidate = path.parent_path() / name;
    if (candidate.wstring().size() >= MAX_PATH) { error = "Parent path is too long for a sibling temporary file"; return refuse(); }
    if (fault == Fault::TempCreate) { fail(error, "Cannot create sibling temporary file", ERROR_WRITE_FAULT); return refuse(); }
    temp.file.value = CreateFileW(candidate.c_str(), GENERIC_WRITE | READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                                   0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temp.file.value != INVALID_HANDLE_VALUE) { temp.path = candidate; break; }
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) { fail(error, "Cannot create sibling temporary file", code); return refuse(); }
  }
  if (temp.path.empty()) { error = "Cannot create unique sibling temporary file after name collisions"; return refuse(); }
  const auto abort = [&](const char *operation, DWORD code) {
    error = text + ": " + nativeError(operation, code);
    temp.cleanup(error, fault);
    return false;
  };
  const bool partial = fault == Fault::Write || fault == Fault::Cleanup;
  const size_t length = partial ? body.size() / 2 : body.size();
  size_t offset = 0;
  while (offset < length) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(length - offset, 1024 * 1024));
    DWORD written = 0;
    if (!WriteFile(temp.file.value, body.data() + offset, chunk, &written, nullptr)) return abort("Cannot write temporary file", GetLastError());
    if (!written) return abort("Cannot write temporary file", ERROR_WRITE_FAULT);
    offset += written;
  }
  if (partial) return abort("Cannot write temporary file", ERROR_WRITE_FAULT);
  if (present) {
    Security created;
    if (!readSecurity(temp.file.value, created, error)) {
      const std::string reason = error; abort("Cannot inspect temporary security", ERROR_ACCESS_DENIED); error += "; " + reason; return false;
    }
    if (!EqualSid(security.owner, created.owner) || !EqualSid(security.group, created.group))
      return abort("Unsupported destination owner or group", ERROR_NOT_SUPPORTED);
    DWORD code = fault == Fault::PermissionCopy ? ERROR_ACCESS_DENIED : SetSecurityInfo(temp.file.value, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | ((security.control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
        nullptr, nullptr, security.dacl, nullptr);
    if (code) return abort("Cannot copy destination permissions", code);
    FILE_BASIC_INFO basic{};
    basic.FileAttributes = info.FileAttributes & ordinaryAttributes;
    if (!SetFileInformationByHandle(temp.file.value, FileBasicInfo, &basic, sizeof(basic)))
      return abort("Cannot copy destination attributes", GetLastError());
  }
  if (fault == Fault::Flush || !FlushFileBuffers(temp.file.value))
    return abort("Cannot flush temporary file", fault == Fault::Flush ? ERROR_WRITE_FAULT : GetLastError());
  const bool closed = temp.file.close();
  if (fault == Fault::Close || !closed) return abort("Cannot close temporary file", fault == Fault::Close ? ERROR_WRITE_FAULT : GetLastError());
  // Metadata/preflight handles must be gone before publication. No post-rename
  // failure may be reported as if the original destination were still present.
  if (present && !destination.close()) return abort("Cannot close destination inspection", GetLastError());
  if (fault == Fault::Replace || !MoveFileExW(temp.path.c_str(), path.c_str(), present ? MOVEFILE_REPLACE_EXISTING : 0))
    return abort("Cannot replace destination", fault == Fault::Replace ? ERROR_WRITE_FAULT : GetLastError());
  temp.path.clear();
  return true;
}
#endif
