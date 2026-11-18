#include "SidecarFileTests.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

int main() {
  if (GetACP() != CP_UTF8) {
    std::fputs("Windows persistence tests require the application's UTF-8 process manifest.\n", stderr);
    return 1;
  }
  return testSidecarFiles() ? 0 : 1;
}
