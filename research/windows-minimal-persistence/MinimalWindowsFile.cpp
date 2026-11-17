// Research-only experimental minimal Windows writer. Not linked into RawNode.
//
// Contract: publish complete contents atomically with a same-directory
// MoveFileExW rename; leave the original untouched on ordinary failure; keep
// refusing files the user may not write; carry over the destination's DACL
// and its visible attributes; report failures. Nothing else is preserved.
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>

#include "MinimalWindowsFile.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <random>

namespace minimal {
namespace {
constexpr DWORD shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  ~Handle() { close(); }
  HANDLE get() const { return value_; }
  bool valid() const { return value_ != INVALID_HANDLE_VALUE; }
  void reset(HANDLE value) { close(); value_ = value; }
  // False only when an open handle fails to close.
  bool close() {
    if (!valid()) return true;
    const BOOL closed = CloseHandle(value_);
    value_ = INVALID_HANDLE_VALUE;
    return closed != FALSE;
  }
 private:
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

std::string utf8(const std::wstring &text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(size, 0)), '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

bool refuse(std::string &error, const std::wstring &path, const std::string &reason) {
  error = utf8(path) + ": " + reason;
  return false;
}

bool fail(std::string &error, const std::wstring &path, const char *operation, DWORD code) {
  wchar_t *message = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
  std::wstring text = message ? message : L"";
  if (message) LocalFree(message);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
  return refuse(error, path, std::string(operation) + " (Windows error " + std::to_string(code) + ")" +
                                 (text.empty() ? "" : ": " + utf8(text)));
}

// The sibling is ours from CREATE_NEW until publication renames it away.
struct Temporary {
  Handle file;
  std::wstring path;
  ~Temporary() {
    file.close();
    if (!path.empty()) DeleteFileW(path.c_str());
  }
};
}  // namespace

bool replaceFile(const std::string &utf8Path, const std::string &body, std::string &error, Options options) {
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(), int(utf8Path.size()), nullptr, 0);
  if (utf8Path.empty() || size <= 0 || utf8Path.find('\0') != std::string::npos) {
    error = utf8Path + ": Invalid UTF-8 path";
    return false;
  }
  std::wstring path(size_t(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(), int(utf8Path.size()), path.data(), size);
  return replaceFile(path, body, error, options);
}

bool replaceFile(const std::wstring &path, const std::string &body, std::string &error, Options options) {
  error.clear();
  std::wstring directory = std::filesystem::path(path).parent_path().wstring();
  if (directory.empty()) directory = L".";

  // 1. Location: the folder that will hold the sibling, following links, so a
  // junction or mount point is judged by the volume actually written.
  Handle folder(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, shareAll, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!folder.valid()) return fail(error, path, "Cannot open destination folder", GetLastError());
  wchar_t filesystem[MAX_PATH + 1] = {};
  DWORD volumeFlags = 0;
  if (!GetVolumeInformationByHandleW(folder.get(), nullptr, 0, nullptr, nullptr, &volumeFlags, filesystem, MAX_PATH + 1))
    return fail(error, path, "Cannot identify destination volume", GetLastError());
  FILE_REMOTE_PROTOCOL_INFO remote{};
  const bool network = GetFileInformationByHandleEx(folder.get(), FileRemoteProtocolInfo, &remote, sizeof remote) != FALSE;
  if (options.localNtfsOnly && (network || std::wcscmp(filesystem, L"NTFS") != 0))
    return refuse(error, path, "Saving is supported only on local NTFS volumes");
  const bool hasAcls = (volumeFlags & FILE_PERSISTENT_ACLS) != 0;
  folder.close();

  // 2. Destination: inspect without following a final link.
  Handle destination(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, shareAll, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  const DWORD opened = destination.valid() ? ERROR_SUCCESS : GetLastError();
  if (opened != ERROR_SUCCESS && opened != ERROR_FILE_NOT_FOUND)
    return fail(error, path, "Cannot inspect destination", opened);
  const bool present = destination.valid();
  DWORD attributes = 0;
  std::unique_ptr<void, decltype(&LocalFree)> descriptor(nullptr, &LocalFree);
  PACL dacl = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  if (present) {
    FILE_BASIC_INFO info{};  // FileAttributeTagInfo is unsupported on FAT32/exFAT (error 87).
    if (!GetFileInformationByHandleEx(destination.get(), FileBasicInfo, &info, sizeof info))
      return fail(error, path, "Cannot inspect destination", GetLastError());
    attributes = info.FileAttributes;
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      return refuse(error, path, "Destination is not a regular file");
    if (attributes & FILE_ATTRIBUTE_READONLY) return refuse(error, path, "Destination is read-only");
    // Rename needs only DELETE access; keep POSIX's refusal of unwritable files.
    Handle writable(CreateFileW(path.c_str(), FILE_WRITE_DATA, shareAll, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!writable.valid()) return fail(error, path, "Destination is not writable", GetLastError());
    if (hasAcls) {
      PSECURITY_DESCRIPTOR raw = nullptr;
      const DWORD code = GetSecurityInfo(destination.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, &dacl, nullptr, &raw);
      descriptor.reset(raw);
      DWORD revision = 0;
      if (code != ERROR_SUCCESS) return fail(error, path, "Cannot read destination permissions", code);
      if (!GetSecurityDescriptorControl(raw, &control, &revision))
        return fail(error, path, "Cannot read destination permissions", GetLastError());
    }
  }

  // 3. Sibling: created exclusively and unshared while it is written.
  Temporary temp;
  std::random_device random;
  for (int attempt = 0; attempt < 8 && !temp.file.valid(); ++attempt) {
    wchar_t name[40];
    std::swprintf(name, 40, L".rawnode-tmp-%08x%08x", unsigned(random()), unsigned(random()));
    const std::wstring candidate = (std::filesystem::path(directory) / name).wstring();
    temp.file.reset(CreateFileW(candidate.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | WRITE_DAC, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    const DWORD code = GetLastError();
    if (temp.file.valid()) temp.path = candidate;
    else if (code != ERROR_FILE_EXISTS) return fail(error, path, "Cannot create sibling temporary file", code);
  }
  if (!temp.file.valid()) return refuse(error, path, "Cannot create a unique sibling temporary file");
  const auto abort = [&](const char *operation, DWORD code) {
    fail(error, path, operation, code);
    if (!temp.file.close()) error += "; cannot close temporary file";
    const bool removed = options.fault != Fault::Cleanup && DeleteFileW(temp.path.c_str());
    if (!removed) error += "; temporary file remains: " + utf8(temp.path);
    temp.path.clear();
    return false;
  };

  // An individually encrypted file must not be republished as plaintext. A
  // sibling in an encrypted folder is encrypted automatically and passes.
  if (attributes & FILE_ATTRIBUTE_ENCRYPTED) {
    FILE_BASIC_INFO created{};
    if (!GetFileInformationByHandleEx(temp.file.get(), FileBasicInfo, &created, sizeof created))
      return abort("Cannot inspect temporary file", GetLastError());
    if (!(created.FileAttributes & FILE_ATTRIBUTE_ENCRYPTED))
      return abort("Encrypted destination would be replaced by an unencrypted file", ERROR_ENCRYPTION_FAILED);
  }

  // 4. Contents. Write/Cleanup faults leave genuine partial data before failing.
  const bool partial = options.fault == Fault::Write || options.fault == Fault::Cleanup;
  const size_t length = partial ? body.size() / 2 : body.size();
  for (size_t offset = 0; offset < length;) {
    const DWORD chunk = DWORD(std::min<size_t>(length - offset, size_t(1) << 20));
    DWORD written = 0;
    if (!WriteFile(temp.file.get(), body.data() + offset, chunk, &written, nullptr))
      return abort("Cannot write temporary file", GetLastError());
    if (written == 0) return abort("Cannot write temporary file", ERROR_WRITE_FAULT);
    offset += written;
  }
  if (partial) return abort("Cannot write temporary file", ERROR_WRITE_FAULT);

  // 5. Carry over what the user controls: permissions and visible attributes.
  // ARCHIVE is set because the contents changed; owner, streams and system
  // ACL entries follow ordinary Windows creation rules, as POSIX rename does.
  if (present && hasAcls) {
    const SECURITY_INFORMATION inheritance = (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                                                           : UNPROTECTED_DACL_SECURITY_INFORMATION;
    const DWORD code = SetSecurityInfo(temp.file.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | inheritance,
                                       nullptr, nullptr, dacl, nullptr);
    if (code != ERROR_SUCCESS) return abort("Cannot copy destination permissions", code);
  }
  const DWORD kept = attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
  if (kept) {
    FILE_BASIC_INFO basic{};  // Zero timestamps are left unchanged.
    basic.FileAttributes = kept | FILE_ATTRIBUTE_ARCHIVE;
    if (!SetFileInformationByHandle(temp.file.get(), FileBasicInfo, &basic, sizeof basic))
      return abort("Cannot copy destination attributes", GetLastError());
  }

  if (options.fault == Fault::Flush || !FlushFileBuffers(temp.file.get()))
    return abort("Cannot flush temporary file", options.fault == Fault::Flush ? ERROR_WRITE_FAULT : GetLastError());
  if (!temp.file.close()) return abort("Cannot close temporary file", GetLastError());
  if (options.fault == Fault::Close) return abort("Cannot close temporary file", ERROR_WRITE_FAULT);

  // 6. Publish. Any open handle on the destination, ours included, would make
  // replacement fail, so the inspection handle closes first.
  destination.close();
  if (options.fault == Fault::Replace ||
      !MoveFileExW(temp.path.c_str(), path.c_str(), present ? MOVEFILE_REPLACE_EXISTING : 0))
    return abort("Cannot replace destination", options.fault == Fault::Replace ? ERROR_WRITE_FAULT : GetLastError());
  temp.path.clear();
  return true;
}
}  // namespace minimal
#endif
