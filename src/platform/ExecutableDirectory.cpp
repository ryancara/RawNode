#include "platform/ExecutableDirectory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

fs::path executableDirectory() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return fs::path(buf).parent_path();
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t sz = sizeof(buf);
  if (_NSGetExecutablePath(buf, &sz) != 0) return {};
  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs::path(buf), ec);
  return (ec ? fs::path(buf) : p).parent_path();
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  return fs::path(buf).parent_path();
#endif
}
