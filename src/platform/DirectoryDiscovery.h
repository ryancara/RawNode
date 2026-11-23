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
DirectoryDiscovery discoverDirectory(const std::filesystem::path &root, std::string_view suffix);
