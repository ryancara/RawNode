#include "platform/FileTrash.h"

#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <versionhelpers.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <sstream>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#if defined(__linux__)
#include <gio/gio.h>
#endif
#endif

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
namespace {
// One file per shell operation. A successful outer HRESULT alone says nothing
// about a skipped/cancelled file. Veto permanent deletion before it can occur.
class RecycleSink final : public IFileOperationProgressSink {
 public:
  bool recycled = false;
  HRESULT itemResult = E_PENDING;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_IFileOperationProgressSink) return E_NOINTERFACE;
    *out = static_cast<IFileOperationProgressSink *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = InterlockedDecrement(&references_);
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem *) override {
    return (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) ? S_OK : E_ABORT;
  }
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT result, IShellItem *created) override {
    itemResult = result;
    recycled = result == S_OK && created != nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return E_ABORT; }
  HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
 private:
  LONG references_ = 1;
};
}

bool moveFileToTrash(const std::string &path, std::string &error) {
  // FOFX_RECYCLEONDELETE requires Windows 8+. Refuse older shells rather than
  // allowing an ignored flag to weaken recoverability.
  if (!IsWindows8OrGreater()) { error = "Recoverable Trash requires Windows 8 or later"; return false; }
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) { error = "Cannot initialize the shell Trash operation"; return false; }
  IFileOperation *operation = nullptr;
  IShellItem *item = nullptr;
  auto *sink = new RecycleSink;
  HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
  if (SUCCEEDED(result)) result = operation->SetOperationFlags(
      FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE | FOF_NOERRORUI | FOF_SILENT |
      FOF_NOCONFIRMATION | FOF_NO_CONNECTED_ELEMENTS | FOF_NORECURSION);
  const auto wide = std::filesystem::path(path).wstring();
  if (SUCCEEDED(result)) result = SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&item));
  if (SUCCEEDED(result)) result = operation->DeleteItem(item, sink);
  if (SUCCEEDED(result)) result = operation->PerformOperations();
  BOOL aborted = TRUE;
  const HRESULT abortResult = operation ? operation->GetAnyOperationsAborted(&aborted) : E_FAIL;
  // PostDeleteItem identifies a completed recycle even if the shell subsequently
  // reports cancellation. No second file is queued in this operation.
  const bool moved = sink->recycled;
  if (!moved) {
    std::ostringstream message;
    message << "Recycle Bin operation failed or was cancelled (shell=0x" << std::hex << result
            << ", item=0x" << sink->itemResult << ", aborted=" << (FAILED(abortResult) || aborted) << ')';
    error = message.str();
  } else error.clear();
  if (item) item->Release();
  if (operation) operation->Release();
  sink->Release();
  CoUninitialize();
  return moved;
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
