#include "ui/CtlScriptDiscovery.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace fs = std::filesystem;

namespace ui {
namespace {

int lower(unsigned char c) { return std::tolower(c); }

bool alphabetical(const std::string &left, const std::string &right) {
  const auto [a, b] = std::mismatch(left.begin(), left.end(), right.begin(), right.end(),
                                    [](char x, char y) { return lower(x) == lower(y); });
  if (a == left.end() || b == right.end()) {
    if (left.size() != right.size()) return left.size() < right.size();
    return left < right;
  }
  return lower(*a) < lower(*b);
}

void scanFolder(const fs::path &path, CtlScriptFolder &folder, bool &scanError) {
  std::error_code iterationError;
  for (fs::directory_iterator it(path, iterationError), end; !iterationError && it != end;
       it.increment(iterationError)) {
    const auto &entry = *it;
    std::error_code entryError;
    // Don't descend through directory symlinks, which can lead back into this tree.
    if (fs::is_directory(entry.symlink_status(entryError))) {
      CtlScriptFolder child;
      scanFolder(entry.path(), child, scanError);
      if (!child.folders.empty() || !child.scripts.empty()) {
        child.name = entry.path().filename().u8string();
        folder.folders.push_back(std::move(child));
      }
    } else if (!entryError) {
      auto extension = entry.path().extension().u8string();
      std::transform(extension.begin(), extension.end(), extension.begin(), lower);
      if (extension == ".ctl" && entry.is_regular_file(entryError))
        folder.scripts.push_back(entry.path().filename().u8string());
    }
    if (entryError) scanError = true;
  }
  if (iterationError) scanError = true;

  std::sort(folder.folders.begin(), folder.folders.end(), [](const auto &a, const auto &b) {
    return alphabetical(a.name, b.name);
  });
  std::sort(folder.scripts.begin(), folder.scripts.end(), alphabetical);
}

}  // namespace

CtlScriptDiscovery discoverCtlScripts(const std::string &directory) {
  CtlScriptDiscovery result;
  if (!directory.empty()) {
    result.directory = fs::u8path(directory);
    scanFolder(result.directory, result.contents, result.scanError);
  }
  return result;
}

}  // namespace ui
