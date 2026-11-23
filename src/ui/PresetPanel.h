#pragma once

#include "platform/DirectoryDiscovery.h"

#include <optional>
#include <string>

namespace ui {

struct PresetPanelState {
  void updateListing(bool open, const std::string &directory, bool refresh = false);

  DirectoryFolder contents;
  std::filesystem::path selectedPath;
  std::string feedback;

private:
  std::optional<std::string> scannedDirectory;
};

}  // namespace ui
