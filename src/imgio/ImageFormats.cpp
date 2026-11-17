#include "imgio/ImageIO.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

const std::vector<std::string> &rasterImageExtensions() {
  static const std::vector<std::string> extensions = {
      ".exr", ".tif", ".tiff", ".png", ".jpg", ".jpeg"};
  return extensions;
}

static bool pathHasExtension(const std::string &path,
                             const std::vector<std::string> &extensions) {
  std::string e = fs::path(path).extension().string();
  for (char &ch : e) ch = (char)tolower((unsigned char)ch);
  return std::find(extensions.begin(), extensions.end(), e) != extensions.end();
}

bool isRasterImagePath(const std::string &path) {
  return pathHasExtension(path, rasterImageExtensions());
}

const std::vector<std::string> &rawImageExtensions() {
  static const std::vector<std::string> extensions = {
      ".3fr", ".arw", ".cr2", ".cr3", ".crw", ".dcr", ".dng", ".erf", ".iiq", ".kdc",
      ".mef", ".mos", ".mrw", ".nef", ".nrw", ".orf", ".pef", ".raf", ".raw", ".rwl",
      ".rw2", ".sr2", ".srf", ".srw", ".x3f"};
  return extensions;
}

bool isRawImagePath(const std::string &path) {
  return pathHasExtension(path, rawImageExtensions());
}
