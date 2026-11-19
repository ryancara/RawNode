#pragma once

#include <filesystem>

class CurrentDirectory {
 public:
  explicit CurrentDirectory(const std::filesystem::path &dir) { std::filesystem::current_path(dir); }
  ~CurrentDirectory() { std::filesystem::current_path(previous_); }

  CurrentDirectory(const CurrentDirectory &) = delete;
  CurrentDirectory &operator=(const CurrentDirectory &) = delete;

 private:
  std::filesystem::path previous_ = std::filesystem::current_path();
};
