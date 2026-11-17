// Disposable research backend. Does not replace any production function.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include "MinimalWriter.h"
#include <algorithm>
#include <climits>
#include <cwchar>
#include <memory>
#include <vector>
#include <utility>

namespace experiment {
namespace fs = std::filesystem;
namespace {
struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : h(h) {}
  ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
  bool close() { const HANDLE owned = h; h = INVALID_HANDLE_VALUE; return CloseHandle(owned) != FALSE; }
};
struct Temporary {
  Handle file;
  fs::path path;
  ~Temporary() {
    if (file.h != INVALID_HANDLE_VALUE) file.close();
    if (!path.empty()) DeleteFileW(path.c_str());
  }
};
struct FreeLocal { void operator()(void *p) const { if (p) LocalFree(p); } };
using Memory = std::unique_ptr<void, FreeLocal>;
constexpr DWORD shared = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD inspectFlags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
bool failure(std::string &error, const char *operation, DWORD code) {
  error = std::string(operation) + " (Windows error " + std::to_string(code) + ")"; return false;
}
bool privateDacl(Memory &out, std::string &error) {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h)) return failure(error, "OpenProcessToken", GetLastError());
  DWORD size = 0; GetTokenInformation(token.h, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> bytes(size);
  if (!GetTokenInformation(token.h, TokenUser, bytes.data(), size, &size)) return failure(error, "TokenUser", GetLastError());
  wchar_t *sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid)) return failure(error, "User SID", GetLastError());
  Memory sidMemory(sid);
  const std::wstring text = L"D:P(A;;FA;;;" + std::wstring(sid) + L")(A;;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return failure(error, "Private DACL", GetLastError());
  out.reset(descriptor); return true;
}
}
bool save(const std::string &text, const std::string &body, std::string &error, Policy policy, Fault fault) {
  error.clear();
  if (text.empty() || text.find('\0') != std::string::npos || text.size() > INT_MAX)
    return failure(error, "Empty/NUL/oversized UTF-8 path", ERROR_INVALID_NAME);
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
  if (!count) return failure(error, "Invalid UTF-8", GetLastError());
  std::vector<wchar_t> wide(static_cast<size_t>(count));
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, wide.data(), count)) return failure(error, "UTF-8 conversion", GetLastError());
  const fs::path path = fs::absolute(fs::path(wide.data()));
  if (path.filename().wstring().find(L':') != std::wstring::npos) return failure(error, "Stream destination", ERROR_INVALID_NAME);
  // Follow directory junctions, then qualify the resolved parent handle once.
  Handle parent(CreateFileW(path.parent_path().c_str(), FILE_READ_ATTRIBUTES, shared, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (parent.h == INVALID_HANDLE_VALUE) return failure(error, "Parent", GetLastError());
  BY_HANDLE_FILE_INFORMATION parentInfo{};
  if (!GetFileInformationByHandle(parent.h, &parentInfo)) return failure(error, "Parent attributes", GetLastError());
  if (!(parentInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (parentInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_ENCRYPTED)))
    return failure(error, "Unsupported provider/encrypted parent", ERROR_NOT_SUPPORTED);
  if (policy.qualifyNtfs) {
    wchar_t filesystem[64], resolved[32768];
    if (!GetVolumeInformationByHandleW(parent.h, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 64)) return failure(error, "Parent filesystem", GetLastError());
    const DWORD n = GetFinalPathNameByHandleW(parent.h, resolved, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= 32768) return failure(error, "Resolved parent", GetLastError());
    if (wcscmp(filesystem, L"NTFS") || wcsncmp(resolved, L"\\\\?\\UNC\\", 8) == 0)
      return failure(error, "Unqualified filesystem/network", ERROR_NOT_SUPPORTED);
  }
  Handle destination(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, shared, nullptr, OPEN_EXISTING, inspectFlags, nullptr));
  const DWORD openError = destination.h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  if (openError && openError != ERROR_FILE_NOT_FOUND) return failure(error, "Destination", openError);
  const bool present = !openError;
  BY_HANDLE_FILE_INFORMATION info{};
  Memory sourceSecurity;
  PACL dacl = nullptr, labels = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  if (present) {
    if (GetFileType(destination.h) != FILE_TYPE_DISK || !GetFileInformationByHandle(destination.h, &info))
      return failure(error, "File attributes", GetLastError());
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_READONLY))
      return failure(error, "Read-only/nonregular/provider/encrypted destination", ERROR_ACCESS_DENIED);
    Handle writable(CreateFileW(path.c_str(), FILE_WRITE_DATA, shared, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (writable.h == INVALID_HANDLE_VALUE) return failure(error, "Writable-file preflight", GetLastError());
    if (policy.copyDacl || policy.copyLabel) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    const SECURITY_INFORMATION fields = DACL_SECURITY_INFORMATION | (policy.copyLabel ?
      LABEL_SECURITY_INFORMATION | ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION | PROCESS_TRUST_LABEL_SECURITY_INFORMATION | ACCESS_FILTER_SECURITY_INFORMATION : 0);
    const DWORD code = GetSecurityInfo(destination.h, SE_FILE_OBJECT, fields, nullptr, nullptr, &dacl, &labels, &sd);
    sourceSecurity.reset(sd);
    if (code) return failure(error, "Destination DACL/label", code);
    DWORD revision;
    if (!GetSecurityDescriptorControl(sd, &control, &revision)) return failure(error, "DACL protection", GetLastError());
    // Auditing is intentionally not copied. Preserve ordinary integrity labels;
    // refuse other enforcement metadata rather than manage enterprise policies.
    if (policy.copyLabel && labels) for (DWORD i = 0; i < labels->AceCount; ++i) {
      void *ace = nullptr;
      if (!GetAce(labels, i, &ace)) return failure(error, "Label ACE", GetLastError());
      if (static_cast<ACE_HEADER *>(ace)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
        return failure(error, "Special enforcement metadata", ERROR_NOT_SUPPORTED);
    }
    }
  }
  Memory security;
  if (policy.privateCreation && !privateDacl(security, error)) return false;
  SECURITY_ATTRIBUTES sa{sizeof(sa), security.get(), FALSE};
  Temporary owned;
  fs::path &temporary = owned.path;
  Handle &output = owned.file;
  for (int attempt = 0; attempt < 16; ++attempt) {
    unsigned char random[16];
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return failure(error, "Random temporary", ERROR_GEN_FAILURE);
    std::wstring name = L".rawnode-research-";
    for (auto byte : random) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
    fs::path candidate = path.parent_path() / name;
    const DWORD access = GENERIC_WRITE | READ_CONTROL | WRITE_DAC | ((labels && labels->AceCount) ? WRITE_OWNER : 0);
    output.h = CreateFileW(candidate.c_str(), access, 0,
      policy.privateCreation ? &sa : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output.h != INVALID_HANDLE_VALUE) { temporary = std::move(candidate); break; }
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) return failure(error, "Create sibling", code);
  }
  if (temporary.empty()) return failure(error, "Temporary collisions", ERROR_FILE_EXISTS);
  const auto abort = [&](const char *operation, DWORD code) {
    failure(error, operation, code);
    if (output.h != INVALID_HANDLE_VALUE && !output.close()) error += "; close error " + std::to_string(GetLastError());
    const BOOL removed = fault == Fault::Cleanup ? FALSE : DeleteFileW(temporary.c_str());
    const DWORD cleanupError = fault == Fault::Cleanup ? ERROR_ACCESS_DENIED : GetLastError();
    if (!removed) error += "; cleanup " + temporary.u8string() + " error " + std::to_string(cleanupError);
    temporary.clear(); // Reported cleanup failures must not be silently retried.
    return false;
  };
  const bool partial = fault == Fault::Write || fault == Fault::Cleanup;
  const size_t length = partial ? body.size() / 2 : body.size();
  for (size_t offset = 0; offset < length;) {
    const DWORD chunk = DWORD(std::min<size_t>(length - offset, 1024 * 1024)); DWORD written = 0;
    if (!WriteFile(output.h, body.data() + offset, chunk, &written, nullptr) || !written) return abort("Write", GetLastError());
    offset += written;
  }
  if (partial) return abort("Injected partial write", ERROR_WRITE_FAULT);
  if (present) {
    if (policy.copyDacl) {
      const DWORD code = SetSecurityInfo(output.h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
        ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION), nullptr, nullptr, dacl, nullptr);
      if (code) return abort("Copy DACL", code);
    }
    if (policy.copyLabel && labels && labels->AceCount) {
      const DWORD code = SetSecurityInfo(output.h, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, labels);
      if (code) return abort("Copy integrity label", code);
    }
    FILE_BASIC_INFO attributes{};
    attributes.FileAttributes = (info.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)) | FILE_ATTRIBUTE_ARCHIVE;
    if (!SetFileInformationByHandle(output.h, FileBasicInfo, &attributes, sizeof(attributes))) return abort("Attributes", GetLastError());
  }
  if (fault == Fault::Flush || !FlushFileBuffers(output.h)) return abort("Flush", fault == Fault::Flush ? ERROR_WRITE_FAULT : GetLastError());
  const bool closed = output.close();
  if (fault == Fault::Close || !closed) return abort("Close", fault == Fault::Close ? ERROR_WRITE_FAULT : GetLastError());
  if (present && !destination.close()) return abort("Close inspection", GetLastError());
  if (fault == Fault::Rename || !MoveFileExW(temporary.c_str(), path.c_str(), present ? MOVEFILE_REPLACE_EXISTING : 0))
    return abort("Rename", fault == Fault::Rename ? ERROR_WRITE_FAULT : GetLastError());
  temporary.clear();
  return true;
}
}
