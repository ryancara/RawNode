#include "ui/PresetPanel.h"
#include "ui/UiContext.h"

#include "imgui.h"

#include <utility>

namespace fs = std::filesystem;

namespace {

void drawFolder(const DirectoryFolder &folder, fs::path &selectedPath) {
  for (const auto &child : folder.folders) {
    const auto name = child.path.filename().u8string();
    if (ImGui::TreeNode(name.c_str())) {
      drawFolder(child, selectedPath);
      ImGui::TreePop();
    }
  }
  for (const auto &preset : folder.files) {
    const auto name = preset.filename().u8string();
    if (ImGui::Selectable(name.c_str(), selectedPath == preset)) selectedPath = preset;
  }
}

}  // namespace

void ui::PresetPanelState::updateListing(bool open, const std::string &directory, bool refresh) {
  if (!open) {
    // Reopening scans again even when the configured root is unchanged.
    scannedDirectory.reset();
    return;
  }
  if (!refresh && scannedDirectory == directory) return;

  scannedDirectory = directory;
  selectedPath.clear();
  contents = {};
  if (directory.empty()) {
    feedback = "Choose a preset directory in File > Settings.";
    return;
  }
  feedback = directoryPreferenceError(directory);
  if (!feedback.empty()) return;

  auto discovery = discoverDirectory(fs::u8path(directory), ".rawnode.json");
  contents = std::move(discovery.contents);
  if (discovery.scanError) {
    feedback = contents.empty() ? "Could not list presets." : "Some preset locations could not be read.";
  } else if (contents.empty()) {
    feedback = "No presets found.";
  }
}

void drawPresetPanel(App &app) {
  auto &panel = app.presetPanel;
  if (!app.showPresetPanel) {
    panel.updateListing(false, app.presetDirectory);
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
  const bool draw = ImGui::Begin("Preset Panel", &app.showPresetPanel);
  const bool refresh = draw && ImGui::Button("Refresh");
  panel.updateListing(app.showPresetPanel, app.presetDirectory, refresh);
  if (draw && app.showPresetPanel) {
    ImGui::SameLine();
    ImGui::BeginDisabled(panel.selectedPath.empty() || !app.renderer.canEditParameters());
    if (ImGui::Button("Replace Grade")) loadPresetDialog(app, false, panel.selectedPath.u8string());
    ImGui::SameLine();
    if (ImGui::Button("Append")) loadPresetDialog(app, true, panel.selectedPath.u8string());
    ImGui::EndDisabled();
    if (!panel.selectedPath.empty()) {
      const auto relativePath = panel.selectedPath.lexically_relative(panel.contents.path).generic_u8string();
      ImGui::TextWrapped("Selected: %s", relativePath.c_str());
    }
    if (!panel.feedback.empty()) ImGui::TextWrapped("%s", panel.feedback.c_str());
    if (ImGui::BeginChild("Presets")) drawFolder(panel.contents, panel.selectedPath);
    ImGui::EndChild();
  }
  ImGui::End();
}
