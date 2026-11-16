#include "platform/FileTrash.h"

#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#if defined(__linux__)
#include <gio/gio.h>
#endif
#endif

bool fileTrashSupported() {
#if defined(__APPLE__) || defined(__linux__)
  return true;
#else
  return false;
#endif
}

bool inspectTrashPath(const std::string &path, bool directory, TrashFileIdentity &identity, std::string &error) {
#if defined(_WIN32)
  const auto wide = std::filesystem::path(path).wstring();
  HANDLE handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "Cannot inspect path (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  const bool read = GetFileInformationByHandle(handle, &info) != FALSE;
  const DWORD failure = read ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!read || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) {
    error = "Not an eligible non-reparse " + std::string(directory ? "folder" : "file") +
        " (Windows error " + std::to_string(failure) + ")";
    return false;
  }
  identity = {info.dwVolumeSerialNumber, (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
      directory ? 0 : (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
      directory ? 0 : (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime};
#else
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) { error = std::strerror(errno); return false; }
  if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) {
    error = directory ? "Not a non-symlink folder" : "Not a regular non-symlink file";
    return false;
  }
#if defined(__APPLE__)
  const auto modified = info.st_mtimespec;
#else
  const auto modified = info.st_mtim;
#endif
  identity = {uint64_t(info.st_dev), uint64_t(info.st_ino), directory ? 0 : uint64_t(info.st_size),
      directory ? 0 : uint64_t(modified.tv_sec) * 1000000000ULL + uint64_t(modified.tv_nsec)};
#endif
  error.clear();
  return true;
}

#if defined(_WIN32)
bool moveFileToTrash(const std::string &, std::string &error) {
  // Native tests reproduced permanent deletion despite recycle-intent callbacks.
  // Keep Windows unsupported until recycling or preservation is guaranteed.
  error = "Move to Trash is temporarily unsupported on Windows until safe recycling is available";
  return false;
}
#elif defined(__linux__)
bool moveFileToTrash(const std::string &path, std::string &error) {
  GFile *file = g_file_new_for_path(path.c_str());
  GError *failure = nullptr;
  const bool moved = g_file_trash(file, nullptr, &failure);
  error = failure ? failure->message : moved ? "" : "Native Trash is unavailable";
  if (failure) g_error_free(failure);
  g_object_unref(file);
  return moved;
}
#elif !defined(__APPLE__)
bool moveFileToTrash(const std::string &, std::string &error) {
  error = "Recoverable Trash is not supported on this platform";
  return false;
}
#endif
