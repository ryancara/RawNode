#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ui {

struct PresetFolder {
  std::filesystem::path path;
  std::vector<PresetFolder> folders;
  std::vector<std::filesystem::path> presets;

  bool empty() const { return folders.empty() && presets.empty(); }
};

struct PresetPanelState {
  void update(bool visible, const std::string &directory, bool refresh = false);

  PresetFolder contents;
  std::filesystem::path selectedPath;
  std::string feedback;

private:
  std::optional<std::string> scannedDirectory;
};

}  // namespace ui
