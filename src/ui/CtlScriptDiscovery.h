#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ui {

struct CtlScriptFolder {
  std::string name;
  std::vector<CtlScriptFolder> folders;
  std::vector<std::string> scripts;
};

struct CtlScriptDiscovery {
  std::filesystem::path directory;
  CtlScriptFolder contents;
  bool scanError = false;
};

CtlScriptDiscovery discoverCtlScripts(const std::string &directory);

}  // namespace ui
