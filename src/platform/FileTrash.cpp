#include "platform/FileTrash.h"

#include <filesystem>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <sherrors.h>
#include <wrl/client.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#if defined(__linux__)
#include <gio/gio.h>
#endif
#endif

bool fileTrashSupported() {
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
  return true;
#else
  return false;
#endif
}

bool inspectTrashPath(const std::string &path, bool directory, TrashFileIdentity &identity, std::string &error) {
#if defined(_WIN32)
  const auto wide = std::filesystem::u8path(path).wstring();
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
struct TrashApartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ~TrashApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// Observe the actual item result, including successful HRESULTs other than S_OK.
// Windows owns confirmation UI; none of these callbacks answers its dialogs.
class DeleteSink final : public IFileOperationProgressSink {
 public:
  HRESULT result = E_PENDING;
  bool recycled = false;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_IFileOperationProgressSink) return E_NOINTERFACE;
    *out = static_cast<IFileOperationProgressSink *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = InterlockedDecrement(&references_);
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT status, IShellItem *created) override {
    result = status;
    recycled = SUCCEEDED(status) && created != nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
 private:
  LONG references_ = 1;
};

bool userCancelled(HRESULT result) {
  return result == COPYENGINE_E_USER_CANCELLED || result == COPYENGINE_S_USER_IGNORED ||
      result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || result == E_ABORT;
}
}  // namespace

TrashOutcome moveFileToTrash(const std::string &path, std::string &error) {
  error.clear();
  if (path.empty() || path.find('\0') != std::string::npos) {
    error = "Invalid Trash path";
    return TrashOutcome::Failed;
  }
  std::filesystem::path source;
  try { source = std::filesystem::absolute(std::filesystem::u8path(path)); }
  catch (const std::system_error &failure) { error = failure.what(); return TrashOutcome::Failed; }
  TrashApartment apartment;
  Microsoft::WRL::ComPtr<IFileOperation> operation;
  Microsoft::WRL::ComPtr<IShellItem> item;
  Microsoft::WRL::ComPtr<DeleteSink> sink;
  sink.Attach(new DeleteSink);
  HRESULT result = apartment.result;
  if (SUCCEEDED(result)) result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
  // Never use FOF_NOCONFIRMATION: it answers Yes to permanent-deletion warnings.
  if (SUCCEEDED(result)) result = operation->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING |
      FOF_NO_CONNECTED_ELEMENTS | FOF_NORECURSION | FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE);
  if (SUCCEEDED(result)) result = operation->SetOwnerWindow(GetActiveWindow());
  if (SUCCEEDED(result)) result = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&item));
  if (SUCCEEDED(result)) result = operation->DeleteItem(item.Get(), sink.Get());
  BOOL aborted = FALSE;
  HRESULT abortResult = S_OK;
  if (SUCCEEDED(result)) {
    result = operation->PerformOperations();
    abortResult = operation->GetAnyOperationsAborted(&aborted);
  }
  // Completed work stays completed even if the aggregate operation reports an error.
  if (sink->recycled) return TrashOutcome::Trashed;
  if (userCancelled(sink->result) || userCancelled(result)) return TrashOutcome::Cancelled;
  if (SUCCEEDED(sink->result)) {
    std::error_code ec;
    if (std::filesystem::symlink_status(source, ec).type() == std::filesystem::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory)) return TrashOutcome::PermanentlyDeleted;
  }
  std::ostringstream message;
  message << "Windows Trash failed (shell=0x" << std::hex << result << ", item=0x" << sink->result
          << ", abort query=0x" << abortResult << ", aborted=" << aborted << ')';
  error = message.str();
  return TrashOutcome::Failed;
}
#elif defined(__linux__)
TrashOutcome moveFileToTrash(const std::string &path, std::string &error) {
  GFile *file = g_file_new_for_path(path.c_str());
  GError *failure = nullptr;
  const bool moved = g_file_trash(file, nullptr, &failure);
  error = failure ? failure->message : moved ? "" : "Native Trash is unavailable";
  if (failure) g_error_free(failure);
  g_object_unref(file);
  return moved ? TrashOutcome::Trashed : TrashOutcome::Failed;
}
#elif !defined(__APPLE__)
TrashOutcome moveFileToTrash(const std::string &, std::string &error) {
  error = "Recoverable Trash is not supported on this platform";
  return TrashOutcome::Failed;
}
#endif
