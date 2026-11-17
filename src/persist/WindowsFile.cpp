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
#include <climits>
#include <cwchar>
#include <memory>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD inspectFlags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
constexpr DWORD keptAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  ~Handle() { close(); }
  bool close() {
    if (value == INVALID_HANDLE_VALUE) return true;
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
  return std::string(operation) + " (Windows error " + std::to_string(code) + ")" + (text.empty() ? "" : ": " + text);
}
bool fail(std::string &error, const char *operation, DWORD code) {
  error = nativeError(operation, code);
  return false;
}
bool attributes(HANDLE file, DWORD &out, std::string &error) {
  FILE_BASIC_INFO info{};
  if (!GetFileInformationByHandleEx(file, FileBasicInfo, &info, sizeof(info)))
    return fail(error, "Cannot inspect file attributes", GetLastError());
  out = info.FileAttributes;
  return true;
}

struct Destination {
  Handle file;
  DWORD attributes = 0;
  bool hasAcls = false;
  LocalMemory security;
  PACL dacl = nullptr, labels = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control = 0;
};

// Inspect the followed parent once. Junctions are judged by their real volume;
// stable directories are part of the existing single-session save contract.
// This is also the non-mutating check used before making a photo editable.
bool inspect(const fs::path &path, Destination &out, std::string &error) {
  Handle parent(CreateFileW(path.parent_path().c_str(), FILE_READ_ATTRIBUTES | FILE_ADD_FILE, sharing,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (parent.value == INVALID_HANDLE_VALUE) return fail(error, "Cannot use destination folder", GetLastError());
  wchar_t filesystem[64];
  DWORD volumeFlags = 0, parentAttributes = 0;
  if (!GetVolumeInformationByHandleW(parent.value, nullptr, 0, nullptr, nullptr, &volumeFlags, filesystem, 64))
    return fail(error, "Cannot identify destination filesystem", GetLastError());
  FILE_REMOTE_PROTOCOL_INFO remote{};
  if (GetFileInformationByHandleEx(parent.value, FileRemoteProtocolInfo, &remote, sizeof(remote))) {
    error = "Network persistence is not qualified";
    return false;
  }
  const DWORD remoteError = GetLastError();
  // The qualified local filesystems return INVALID_PARAMETER for this class.
  // An unsupported/unknown protocol query does not establish local storage.
  if (remoteError != ERROR_INVALID_PARAMETER) return fail(error, "Cannot qualify local storage", remoteError);
  if (std::wcscmp(filesystem, L"NTFS") && std::wcscmp(filesystem, L"FAT32") && std::wcscmp(filesystem, L"exFAT")) {
    error = "Atomic persistence is qualified only on local NTFS, FAT32 and exFAT";
    return false;
  }
  if (!attributes(parent.value, parentAttributes, error)) return false;
  if (!(parentAttributes & FILE_ATTRIBUTE_DIRECTORY) || (parentAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_ENCRYPTED))) {
    error = "Provider/reparse-backed or encrypted destination folder is not qualified";
    return false;
  }
  out.hasAcls = (volumeFlags & FILE_PERSISTENT_ACLS) != 0;
  out.file.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (out.hasAcls ? READ_CONTROL : 0), sharing,
                               nullptr, OPEN_EXISTING, inspectFlags, nullptr);
  if (out.file.value == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND || fail(error, "Cannot inspect destination", code);
  }
  if (GetFileType(out.file.value) != FILE_TYPE_DISK) { error = "Destination is not a regular disk file"; return false; }
  if (!attributes(out.file.value, out.attributes, error)) return false;
  if (out.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) {
    error = "Destination is not a qualified regular file";
    return false;
  }
  if (out.attributes & FILE_ATTRIBUTE_READONLY) { error = "Destination is read-only"; return false; }
  if (out.attributes & FILE_ATTRIBUTE_ENCRYPTED) {
    error = "Encrypted destination is not qualified for replacement";
    return false;
  }
  // Rename alone can replace a write-denied file. Probe data access without
  // truncation; append access is not needed to replace a JSON data stream.
  Handle writable(CreateFileW(path.c_str(), FILE_WRITE_DATA, sharing, nullptr, OPEN_EXISTING, inspectFlags, nullptr));
  if (writable.value == INVALID_HANDLE_VALUE) return fail(error, "Destination is not writable", GetLastError());
  if (!out.hasAcls) return true;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  constexpr SECURITY_INFORMATION fields = DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION |
      ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION | PROCESS_TRUST_LABEL_SECURITY_INFORMATION | ACCESS_FILTER_SECURITY_INFORMATION;
  const DWORD code = GetSecurityInfo(out.file.value, SE_FILE_OBJECT, fields, nullptr, nullptr, &out.dacl, &out.labels, &descriptor);
  out.security.reset(descriptor);
  if (code) return fail(error, "Cannot read destination permissions", code);
  DWORD revision;
  if (!GetSecurityDescriptorControl(descriptor, &out.control, &revision))
    return fail(error, "Cannot inspect permission inheritance", GetLastError());
  // Audit ACEs are not requested. Empty filtered audit ACLs are harmless.
  // Copy ordinary integrity restrictions, but do not manage enterprise policy.
  if (out.labels) for (DWORD i = 0; i < out.labels->AceCount; ++i) {
    void *ace = nullptr;
    if (!GetAce(out.labels, i, &ace)) return fail(error, "Cannot inspect integrity label", GetLastError());
    if (static_cast<ACE_HEADER *>(ace)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
      error = "Unsupported specialised access-enforcement metadata";
      return false;
    }
  }
  return true;
}

// Existing restricted contents must remain private even if an interrupted
// write leaves a partial sibling. New files deliberately inherit the folder.
bool privateTemporarySecurity(LocalMemory &out, std::string &error) {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return fail(error, "Cannot read current user", GetLastError());
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (!size || !GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) return fail(error, "Cannot read user SID", GetLastError());
  wchar_t *sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid)) return fail(error, "Cannot encode user SID", GetLastError());
  LocalMemory sidMemory(sid);
  const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    return fail(error, "Cannot create temporary permissions", GetLastError());
  out.reset(descriptor);
  return true;
}
struct Temporary {
  Handle file;
  fs::path path;
  ~Temporary() { file.close(); if (!path.empty()) DeleteFileW(path.c_str()); }
  void cleanup(std::string &error, persist_detail::FileFault fault) {
    if (!file.close()) error += "; " + nativeError("Cannot close temporary file", GetLastError());
    const BOOL removed = fault == persist_detail::FileFault::Cleanup ? FALSE : DeleteFileW(path.c_str());
    const DWORD code = fault == persist_detail::FileFault::Cleanup ? ERROR_ACCESS_DENIED : GetLastError();
    if (!removed) error += "; cannot remove temporary file " + path.u8string() + ": " + nativeError("DeleteFileW", code);
    path.clear(); // Never silently retry a reported cleanup failure.
  }
};
}  // namespace

bool persist_detail::windowsUtf8Environment(std::string &error) {
  if (GetACP() == CP_UTF8) return true;
  error = "RawNode requires the UTF-8 Windows application manifest (Windows 10 1903 or later); refusing mixed-code-page paths";
  return false;
}
bool persist_detail::windowsUtf8(const wchar_t *text, std::string &out, std::string &error) {
  if (!text || !*text) { out.clear(); return true; }
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
  if (!count) { error = "Invalid Unicode path"; return false; }
  std::vector<char> bytes(static_cast<size_t>(count));
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, bytes.data(), count, nullptr, nullptr))
    return fail(error, "Cannot convert Unicode path", GetLastError());
  out.assign(bytes.data(), bytes.size() - 1);
  return true;
}
bool persist_detail::windowsPath(const std::string &text, fs::path &path, std::string &error) {
  if (text.empty() || text.size() >= INT_MAX || text.find('\0') != std::string::npos) { error = "Empty, oversized or embedded-NUL persistence path"; return false; }
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
  if (!count) return fail(error, "Invalid UTF-8 persistence path", GetLastError());
  std::vector<wchar_t> wide(static_cast<size_t>(count));
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, wide.data(), count)) return fail(error, "Cannot convert UTF-8 path", GetLastError());
  const DWORD length = GetFullPathNameW(wide.data(), 0, nullptr, nullptr);
  if (!length) return fail(error, "Cannot resolve persistence path", GetLastError());
  std::vector<wchar_t> absolute(length);
  const DWORD resolved = GetFullPathNameW(wide.data(), length, absolute.data(), nullptr);
  if (!resolved || resolved >= length) return fail(error, "Cannot resolve persistence path", GetLastError());
  path = absolute.data();
  if (path.filename().wstring().find(L':') != std::wstring::npos) { error = "Named-stream destinations are not supported"; return false; }
  return true;
}
bool persist_detail::windowsCanSaveFile(const std::string &text, std::string &error) {
  error.clear();
  fs::path path;
  Destination destination;
  if (windowsPath(text, path, error) && inspect(path, destination, error)) return true;
  error = text + ": " + error;
  return false;
}
bool persist_detail::replaceWindowsFile(const std::string &text, const std::string &body, std::string &error, FileFault fault) {
  using Fault = FileFault;
  error.clear();
  const auto refuse = [&] { error = text + ": " + error; return false; };
  fs::path path;
  Destination destination;
  if (fault == Fault::PermissionStat) { fail(error, "Cannot read destination permissions", ERROR_READ_FAULT); return refuse(); }
  if (!windowsPath(text, path, error) || !inspect(path, destination, error)) return refuse();
  const bool present = destination.file.value != INVALID_HANDLE_VALUE;
  if (present && fault == Fault::PermissionCheck) { fail(error, "Destination is not writable", ERROR_ACCESS_DENIED); return refuse(); }
  const bool restricted = present && destination.hasAcls;
  LocalMemory security;
  if (restricted && !privateTemporarySecurity(security, error)) return refuse();
  SECURITY_ATTRIBUTES sa{sizeof(sa), security.get(), FALSE};
  Temporary temp;
  for (int attempt = 0; attempt < 16; ++attempt) {
    unsigned char random[16];
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) { error = "Cannot generate temporary name"; return refuse(); }
    std::wstring name = L".rawnode-tmp-";
    for (auto byte : random) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
    fs::path candidate = path.parent_path() / name;
    if (fault == Fault::TempCreate) { fail(error, "Cannot create sibling temporary file", ERROR_WRITE_FAULT); return refuse(); }
    const bool hasLabel = destination.labels && destination.labels->AceCount;
    const DWORD access = GENERIC_WRITE | FILE_READ_ATTRIBUTES | (restricted ? WRITE_DAC : 0) | (hasLabel ? WRITE_OWNER : 0);
    temp.file.value = CreateFileW(candidate.c_str(), access, 0, restricted ? &sa : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temp.file.value != INVALID_HANDLE_VALUE) { temp.path = std::move(candidate); break; }
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) { fail(error, "Cannot create sibling temporary file", code); return refuse(); }
  }
  if (temp.path.empty()) { error = "Cannot create a unique sibling temporary file"; return refuse(); }
  const auto abort = [&](const char *operation, DWORD code) {
    error = text + ": " + nativeError(operation, code);
    temp.cleanup(error, fault);
    return false;
  };
  // Establish confidentiality before the first byte, including partial leftovers.
  if (destination.labels && destination.labels->AceCount) {
    const DWORD code = SetSecurityInfo(temp.file.value, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, destination.labels);
    if (code) return abort("Cannot preserve integrity label", code);
  }
  const bool partial = fault == Fault::Write || fault == Fault::Cleanup;
  const size_t length = partial ? body.size() / 2 : body.size();
  for (size_t offset = 0; offset < length;) {
    const DWORD chunk = DWORD(std::min<size_t>(length - offset, 1024 * 1024));
    DWORD written = 0;
    if (!WriteFile(temp.file.value, body.data() + offset, chunk, &written, nullptr)) return abort("Cannot write temporary file", GetLastError());
    if (!written) return abort("Cannot write temporary file", ERROR_WRITE_FAULT);
    offset += written;
  }
  if (partial) return abort("Cannot write temporary file", ERROR_WRITE_FAULT);
  if (restricted) {
    const DWORD code = fault == Fault::PermissionCopy ? ERROR_ACCESS_DENIED : SetSecurityInfo(temp.file.value, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | ((destination.control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
        nullptr, nullptr, destination.dacl, nullptr);
    if (code) return abort("Cannot copy destination permissions", code);
  }
  if (destination.attributes & keptAttributes) {
    FILE_BASIC_INFO basic{};
    basic.FileAttributes = (destination.attributes & keptAttributes) | FILE_ATTRIBUTE_ARCHIVE;
    if (!SetFileInformationByHandle(temp.file.value, FileBasicInfo, &basic, sizeof(basic))) return abort("Cannot copy destination attributes", GetLastError());
  }
  if (fault == Fault::Flush || !FlushFileBuffers(temp.file.value)) return abort("Cannot flush temporary file", fault == Fault::Flush ? ERROR_WRITE_FAULT : GetLastError());
  const bool closed = temp.file.close();
  if (fault == Fault::Close || !closed) return abort("Cannot close temporary file", fault == Fault::Close ? ERROR_WRITE_FAULT : GetLastError());
  if (!destination.file.close()) return abort("Cannot close destination inspection", GetLastError());
  if (fault == Fault::Replace || !MoveFileExW(temp.path.c_str(), path.c_str(), present ? MOVEFILE_REPLACE_EXISTING : 0))
    return abort("Cannot replace destination", fault == Fault::Replace ? ERROR_WRITE_FAULT : GetLastError());
  temp.path.clear();
  return true;
}
#endif
