#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

struct DirectoryFolder {
  std::filesystem::path path;
  std::vector<DirectoryFolder> folders;
  std::vector<std::filesystem::path> files;

  bool empty() const { return folders.empty() && files.empty(); }
};

struct DirectoryDiscovery {
  DirectoryFolder contents;
  bool scanError = false;
};

bool hasFilenameSuffix(std::string_view filename, std::string_view suffix);

// Recursively matches regular files by ASCII case-insensitive filename suffix.
// Sibling names sort alphabetically ignoring ASCII case, with byte-order ties.
// Skips symlink entries and subfolders without matches. The root may be a symlink.
// scanError indicates some or all of the tree could not be read. Available
// results are retained.
DirectoryDiscovery discoverDirectory(const std::filesystem::path &root, std::string_view suffix);
