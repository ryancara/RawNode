// Diagnostic-only: exact production adapter is linked separately from baseline.
// No permanent deletion is authorised by this harness. Only independent copies
// of explicitly acknowledged, byte-verified disposable fixtures are accepted.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include "platform/FileTrash.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
namespace fs = std::filesystem;

static std::string utf8(const std::wstring &text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string value(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), value.data(), size, nullptr, nullptr);
  return value;
}
static std::string quote(const std::string &text) {
  std::ostringstream out; out << '"';
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out << '\\' << char(c);
    else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
    else out << char(c);
  }
  out << '"'; return out.str();
}
static std::string hexValue(DWORD value) {
  std::ostringstream out; out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
  return out.str();
}
static std::string yes(bool value) { return value ? "true" : "false"; }
struct Log {
  std::ofstream file;
  std::mutex mutex;
  explicit Log(const fs::path &path) : file(path, std::ios::binary) {}
  void event(const std::string &name, std::initializer_list<std::pair<std::string,std::string>> fields = {}) {
    std::lock_guard<std::mutex> lock(mutex);
    file << "{\"event\":" << quote(name);
    for (const auto &field : fields) file << ',' << quote(field.first) << ':' << quote(field.second);
    file << "}\n"; file.flush();
  }
};
static std::string itemName(IShellItem *item, SIGDN kind) {
  PWSTR text = nullptr;
  if (!item || FAILED(item->GetDisplayName(kind, &text)) || !text) return {};
  auto result = utf8(text); CoTaskMemFree(text); return result;
}
// This sink uses the same veto, result classification and other callback return
// values as RecycleSink in the pinned production source. Logging adds observation.
class Sink final : public IFileOperationProgressSink {
 public:
  explicit Sink(Log &log, bool forceVeto) : log_(log), forceVeto_(forceVeto) {}
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
    ULONG remaining = InterlockedDecrement(&references_); if (!remaining) delete this; return remaining;
  }
  HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem *item) override {
    HRESULT decision = !forceVeto_ && (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) ? S_OK : E_ABORT;
    log_.event("PreDeleteItem", {{"flags",hexValue(flags)}, {"recycleIntent",yes(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)},
        {"decision",hexValue(decision)}, {"item",itemName(item,SIGDN_FILESYSPATH)}});
    return decision;
  }
  HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD flags, IShellItem *item, HRESULT result, IShellItem *created) override {
    itemResult = result; recycled = result == S_OK && created != nullptr;
    log_.event("PostDeleteItem", {{"flags",hexValue(flags)}, {"result",hexValue(result)},
        {"created",yes(created != nullptr)}, {"recycled",yes(recycled)},
        {"source",itemName(item,SIGDN_FILESYSPATH)}, {"createdPath",itemName(created,SIGDN_FILESYSPATH)},
        {"createdParsingName",itemName(created,SIGDN_DESKTOPABSOLUTEPARSING)}});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StartOperations() override { log_.event("StartOperations"); return S_OK; }
  HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override { log_.event("FinishOperations",{{"result",hexValue(result)}}); return S_OK; }
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
  Log &log_;
  bool forceVeto_;
};

static bool observedOperation(const fs::path &source, bool noConfirmation, bool forceVeto, bool policyControl, Log &log) {
  HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  log.event("CoInitializeEx",{{"result",hexValue(initialized)}});
  if (FAILED(initialized)) return false;
  IFileOperation *operation = nullptr; IShellItem *item = nullptr; auto *sink = new Sink(log,forceVeto);
  HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
  log.event("CoCreateInstance",{{"result",hexValue(result)}});
  DWORD flags = FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE | FOF_NOERRORUI | FOF_SILENT |
      FOF_NO_CONNECTED_ELEMENTS | FOF_NORECURSION;
  if (noConfirmation) flags |= FOF_NOCONFIRMATION;
  if (policyControl) flags = (flags & ~FOFX_RECYCLEONDELETE) | FOF_ALLOWUNDO;
  if (SUCCEEDED(result)) {
    result = operation->SetOperationFlags(flags);
    log.event("SetOperationFlags",{{"flags",hexValue(flags)},{"result",hexValue(result)}});
  }
  if (SUCCEEDED(result)) {
    result = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&item));
    log.event("SHCreateItemFromParsingName",{{"result",hexValue(result)}});
  }
  if (SUCCEEDED(result)) {
    result = operation->DeleteItem(item, sink);
    log.event("DeleteItem",{{"result",hexValue(result)}});
  }
  if (SUCCEEDED(result)) {
    result = operation->PerformOperations();
    log.event("PerformOperations",{{"result",hexValue(result)}});
  }
  BOOL aborted = TRUE;
  HRESULT abortResult = operation ? operation->GetAnyOperationsAborted(&aborted) : E_FAIL;
  log.event("GetAnyOperationsAborted",{{"result",hexValue(abortResult)},{"aborted",yes(aborted != FALSE)}});
  bool moved = sink->recycled;
  log.event("classification",{{"moved",yes(moved)},{"itemResult",hexValue(sink->itemResult)}});
  if (item) item->Release();
  if (operation) operation->Release();
  sink->Release();
  CoUninitialize();
  return moved;
}

// Only Cancel or No is ever automated. An unrecognised/hidden dialog is recorded
// by the parent timeout as inconclusive; this monitor never approves any prompt.
struct DialogMonitor { DWORD process; Log &log; bool cancelled = false; };
static std::wstring windowText(HWND window) {
  wchar_t buffer[2048]{}; GetWindowTextW(window,buffer,2048); return buffer;
}
static BOOL CALLBACK inspectDialog(HWND window, LPARAM context) {
  auto &monitor = *reinterpret_cast<DialogMonitor *>(context);
  DWORD process = 0; GetWindowThreadProcessId(window,&process);
  if (process != monitor.process || !IsWindowVisible(window)) return TRUE;
  wchar_t klass[128]{}; GetClassNameW(window,klass,128);
  if (std::wstring(klass) != L"#32770") return TRUE;
  HWND button = GetDlgItem(window,IDCANCEL);
  if (!button || !IsWindowVisible(button) || !IsWindowEnabled(button)) button = GetDlgItem(window,IDNO);
  if (button && IsWindowVisible(button) && IsWindowEnabled(button) && !monitor.cancelled) {
    monitor.log.event("dialog_cancelled_by_harness",{{"title",utf8(windowText(window))},
        {"button",utf8(windowText(button))},{"buttonId",std::to_string(GetDlgCtrlID(button))}});
    PostMessageW(button,BM_CLICK,0,0); monitor.cancelled = true;
  }
  return TRUE;
}
static bool sameBytes(const fs::path &a, const fs::path &b) {
  if (!fs::is_regular_file(a) || !fs::is_regular_file(b) || fs::file_size(a) != fs::file_size(b)) return false;
  std::ifstream first(a,std::ios::binary), second(b,std::ios::binary);
  char aa[8192],bb[8192];
  while (first) {
    first.read(aa,sizeof aa); second.read(bb,sizeof bb);
    if (first.gcount() != second.gcount() || !std::equal(aa,aa+first.gcount(),bb)) return false;
  }
  return first.eof() && (second.eof() || second.good());
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 6 || std::wstring(argv[5]) != L"RAWNODE_DISPOSABLE_FIXTURE_ACK") return 64;
  fs::path source(argv[2]), backup(argv[3]);
  if (source.filename().wstring().find(L"RawNodeProbe-") != 0 ||
      source.parent_path().wstring().find(L"RawNodeTrashProbe-") == std::wstring::npos || !sameBytes(source,backup)) return 65;
  Log log(argv[4]); if (!log.file) return 66;
  std::wstring mode(argv[1]);
  if (mode != L"production" && mode != L"instrumented" && mode != L"without-noconfirmation" && mode != L"force-veto-control" && mode != L"policy-intent-control") return 67;
  log.event("begin",{{"mode",utf8(mode)},{"source",utf8(source.wstring())},{"backupVerified","true"},{"driveTypeCode",std::to_string(GetDriveTypeW(source.root_path().c_str()))}});
  std::atomic<bool> stop{false};
  DialogMonitor monitor{GetCurrentProcessId(),log};
  std::thread watcher([&] {
    while (!stop) { EnumWindows(inspectDialog,reinterpret_cast<LPARAM>(&monitor)); std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
  });
  bool moved = false;
  if (mode == L"production") {
    std::string error; moved = moveFileToTrash(source.string(),error);
    log.event("production_result",{{"moved",yes(moved)},{"error",error}});
  } else moved = observedOperation(source,mode != L"without-noconfirmation",
      mode == L"force-veto-control" || mode == L"policy-intent-control",mode == L"policy-intent-control",log);
  stop = true; watcher.join();
  log.event("end",{{"moved",yes(moved)},{"originalExists",yes(fs::exists(source))},
      {"cancelledByHarness",yes(monitor.cancelled)}});
  return 0;
}
