#include <cstring>
#include <limits>

// Deliberate, independent faults: CI must reject each under its real runtime
// settings, including the JPEG exception. This executable is never shipped.
__attribute__((noinline)) static void leakAllocation() {
  volatile int *values = new int[19];
  values[0] = 42;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  if (std::strcmp(argv[1], "address") == 0) {
    int *values = new int[argc];
    delete[] values;
    return *static_cast<volatile int *>(values);
  }
  if (std::strcmp(argv[1], "overflow") == 0) {
    volatile int maximum = std::numeric_limits<int>::max();
    return maximum + 1;
  }
  if (std::strcmp(argv[1], "shift") == 0) {
    volatile int negative = -1;
    return negative << 1;
  }
  if (std::strcmp(argv[1], "leak") == 0) {
    leakAllocation();
    return 0;
  }
  return 2;
}
