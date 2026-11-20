// Native fixture driver, linked to the production adapter. The PowerShell test
// owns the fixtures and independently checks the bin, restoration and hashes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "platform/FileTrash.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static std::string utf8(const std::wstring &text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
  return result;
}

static std::string quoted(const std::string &text) {
  std::string result = "\"";
  for (const char c : text) {
    if (c == '\\' || c == '"') result += '\\';
    if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += c;
  }
  return result + '"';
}

static std::wstring windowText(HWND window) {
  wchar_t buffer[2048]{};
  GetWindowTextW(window, buffer, 2048);
  return buffer;
}

struct DialogMonitor {
  DWORD process = GetCurrentProcessId();
  bool close = false, declined = false;
  std::vector<std::string> dialogs;
};

static BOOL CALLBACK collectText(HWND child, LPARAM context) {
  auto &text = *reinterpret_cast<std::wstring *>(context);
  const auto value = windowText(child);
  if (!value.empty()) text += L" | " + value;
  return TRUE;
}

static BOOL CALLBACK declineDialog(HWND window, LPARAM context) {
  auto &monitor = *reinterpret_cast<DialogMonitor *>(context);
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process != monitor.process || !IsWindowVisible(window) || monitor.declined) return TRUE;
  wchar_t kind[128]{};
  GetClassNameW(window, kind, 128);
  if (std::wstring(kind) != L"#32770") return TRUE;
  HWND button = GetDlgItem(window, IDNO);
  if (!button || !IsWindowVisible(button) || !IsWindowEnabled(button)) button = GetDlgItem(window, IDCANCEL);
  if (!button || !IsWindowVisible(button) || !IsWindowEnabled(button)) return TRUE;
  auto text = windowText(window);
  EnumChildWindows(window, collectText, reinterpret_cast<LPARAM>(&text));
  monitor.dialogs.push_back(utf8(text));
  monitor.declined = true;
  // Only a negative response or closing the dialog is allowed. Never click Yes,
  // OK or a default button, including when a dialog's text is unexpected.
  if (monitor.close) PostMessageW(window, WM_CLOSE, 0, 0);
  else PostMessageW(button, BM_CLICK, 0, 0);
  return TRUE;
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 5) return 64;
  const fs::path source(argv[1]), report(argv[2]);
  const std::wstring mode(argv[3]);
  if (std::wstring(argv[4]) != L"RAWNODE_DISPOSABLE_FIXTURE" ||
      source.filename().wstring().find(L"RawNodeTrashFixture-") != 0 ||
      source.parent_path().filename().wstring().find(L"RawNodeTrashTest-") != 0 ||
      (mode != L"no" && mode != L"close" && mode != L"locked")) return 65;
  HANDLE locked = INVALID_HANDLE_VALUE;
  if (mode == L"locked") {
    locked = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return 66;
  }
  DialogMonitor monitor;
  monitor.close = mode == L"close";
  std::atomic<bool> stop{false};
  std::thread watcher([&] {
    while (!stop) {
      EnumWindows(declineDialog, reinterpret_cast<LPARAM>(&monitor));
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  std::string error;
  const auto outcome = moveFileToTrash(source.u8string(), error);
  stop = true;
  watcher.join();
  if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
  const char *name = "failed";
  switch (outcome) {
    case TrashOutcome::Trashed: name = "trashed"; break;
    case TrashOutcome::PermanentlyDeleted: name = "permanently-deleted"; break;
    case TrashOutcome::Cancelled: name = "cancelled"; break;
    case TrashOutcome::Failed: break;
  }
  std::ofstream out(report, std::ios::binary);
  out << "{\"outcome\":" << quoted(name) << ",\"error\":" << quoted(error)
      << ",\"declined\":" << (monitor.declined ? "true" : "false") << ",\"dialogs\":[";
  for (size_t i = 0; i < monitor.dialogs.size(); ++i) {
    if (i) out << ',';
    out << quoted(monitor.dialogs[i]);
  }
  out << "]}\n";
  return out ? 0 : 67;
}
