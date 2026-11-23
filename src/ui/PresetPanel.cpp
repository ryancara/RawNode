#include "ui/PresetPanel.h"
#include "ui/UiContext.h"

#include "imgui.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace {

ui::PresetFolder scanFolder(const fs::path &path, bool &scanError) {
  ui::PresetFolder folder;
  folder.path = path;
  std::error_code iterationError;
  for (fs::directory_iterator it(path, iterationError), end; !iterationError && it != end;
       it.increment(iterationError)) {
    const auto &entry = *it;
    std::error_code entryError;
    // Directory links can cycle, and the preset loader requires a regular file.
    const auto status = entry.symlink_status(entryError);
    if (fs::is_directory(status)) {
      auto child = scanFolder(entry.path(), scanError);
      if (!child.empty()) folder.folders.push_back(std::move(child));
    } else if (fs::is_regular_file(status)) {
      const auto name = entry.path().filename().u8string();
      constexpr std::string_view suffix = ".rawnode.json";
      if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        folder.presets.push_back(entry.path());
    }
    if (entryError) scanError = true;
  }
  if (iterationError) scanError = true;

  std::sort(folder.folders.begin(), folder.folders.end(), [](const auto &left, const auto &right) {
    return left.path < right.path;
  });
  std::sort(folder.presets.begin(), folder.presets.end());
  return folder;
}

void drawFolder(const ui::PresetFolder &folder, fs::path &selectedPath) {
  for (const auto &child : folder.folders) {
    const auto name = child.path.filename().u8string();
    if (ImGui::TreeNode(name.c_str())) {
      drawFolder(child, selectedPath);
      ImGui::TreePop();
    }
  }
  for (const auto &preset : folder.presets) {
    const auto name = preset.filename().u8string();
    if (ImGui::Selectable(name.c_str(), selectedPath == preset)) selectedPath = preset;
  }
}

}  // namespace

void ui::PresetPanelState::update(bool visible, const std::string &directory, bool refresh) {
  if (!visible) {
    scannedDirectory.reset();
    return;
  }
  if (!refresh && scannedDirectory == directory) return;

  scannedDirectory = directory;
  selectedPath.clear();
  contents = {};
  if (directory.empty()) {
    feedback = "Choose a preset directory in File → Settings.";
    return;
  }
  feedback = directoryPreferenceError(directory);
  if (!feedback.empty()) return;

  bool scanError = false;
  contents = scanFolder(fs::u8path(directory), scanError);
  if (scanError) feedback = "Some preset locations could not be read.";
  else if (contents.empty()) feedback = "No presets found.";
}

void drawPresetPanel(App &app) {
  auto &panel = app.presetPanel;
  if (!app.showPresetPanel) {
    panel.update(false, app.presetDirectory);
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
  const bool draw = ImGui::Begin("Preset Panel", &app.showPresetPanel);
  const bool refresh = draw && ImGui::Button("Refresh");
  panel.update(app.showPresetPanel, app.presetDirectory, refresh);
  if (draw && app.showPresetPanel) {
    ImGui::SameLine();
    ImGui::BeginDisabled(panel.selectedPath.empty() || !app.renderer.canEditParameters());
    if (ImGui::Button("Replace Grade")) loadPresetDialog(app, false, panel.selectedPath.u8string());
    ImGui::SameLine();
    if (ImGui::Button("Append")) loadPresetDialog(app, true, panel.selectedPath.u8string());
    ImGui::EndDisabled();
    if (!panel.feedback.empty()) ImGui::TextWrapped("%s", panel.feedback.c_str());
    if (ImGui::BeginChild("Presets")) drawFolder(panel.contents, panel.selectedPath);
    ImGui::EndChild();
  }
  ImGui::End();
}
