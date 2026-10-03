// Minimal still-image processor host: decode RAW/raster, process, preview, export.

#include "UI.h"
#include "SelfTest.h"

#include <cstring>
#include <string>

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--selftest")) return runSelfTests();
  const std::string path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "";
  return runApp(path);
}
