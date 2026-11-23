#include "platform/DirectoryDiscovery.h"

#include <algorithm>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace {

unsigned char lowerAscii(unsigned char ch) {
  return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

bool sameLetter(char left, char right) {
  return lowerAscii(left) == lowerAscii(right);
}

bool alphabetical(const fs::path &left, const fs::path &right) {
  const auto a = left.filename().u8string();
  const auto b = right.filename().u8string();
  const auto [first, second] = std::mismatch(a.begin(), a.end(), b.begin(), b.end(), sameLetter);
  if (first != a.end() && second != b.end()) return lowerAscii(*first) < lowerAscii(*second);
  if (a.size() != b.size()) return a.size() < b.size();
  return a < b;
}

DirectoryFolder scanFolder(const fs::path &path, std::string_view suffix, bool &scanError) {
  DirectoryFolder folder;
  folder.path = path;
  std::error_code iterationError;
  for (fs::directory_iterator it(path, iterationError), end; !iterationError && it != end;
       it.increment(iterationError)) {
    const auto &entry = *it;
    std::error_code entryError;
    // Inspect entries without following links. A directory link could lead back into this tree.
    const auto status = entry.symlink_status(entryError);
    if (entryError) {
      scanError = true;
    } else if (fs::is_directory(status)) {
      auto child = scanFolder(entry.path(), suffix, scanError);
      if (!child.empty()) folder.folders.push_back(std::move(child));
    } else if (fs::is_regular_file(status) && hasFilenameSuffix(entry.path().filename().u8string(), suffix)) {
      folder.files.push_back(entry.path());
    }
  }
  if (iterationError) scanError = true;

  std::sort(folder.folders.begin(), folder.folders.end(), [](const auto &left, const auto &right) {
    return alphabetical(left.path, right.path);
  });
  std::sort(folder.files.begin(), folder.files.end(), alphabetical);
  return folder;
}

}  // namespace

bool hasFilenameSuffix(std::string_view filename, std::string_view suffix) {
  return filename.size() >= suffix.size() &&
         std::equal(suffix.rbegin(), suffix.rend(), filename.rbegin(), sameLetter);
}

DirectoryDiscovery discoverDirectory(const fs::path &root, std::string_view suffix) {
  DirectoryDiscovery result;
  if (!root.empty()) result.contents = scanFolder(root, suffix, result.scanError);
  return result;
}
