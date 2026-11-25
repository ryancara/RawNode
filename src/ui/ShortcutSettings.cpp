#include "ui/UiContext.h"

#include "imgui_internal.h"

#include <algorithm>

namespace ui {
namespace {
constexpr const char *capturePopup = "Assign keyboard shortcut";

void beginCapture(App &app, ImGuiKeyChord replacing = 0) {
  app.settingsWindow.shortcutBeingChanged = replacing;
  app.settingsWindow.shortcutFeedback.clear();
  ImGui::OpenPopup(capturePopup);
}

bool drawCapture(App &app) {
  if (!ImGui::BeginPopupModal(capturePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return false;

  auto &window = app.settingsWindow;
  ImGui::TextUnformatted(commandInfo(window.shortcutCommand).label);
  ImGui::TextUnformatted("Press a key or key combination. Escape cancels.");

  bool close = false;
  for (int index = ImGuiKey_Tab; index <= ImGuiKey_KeypadEqual; ++index) {
    const auto key = static_cast<ImGuiKey>(index);
    if (keyboardKeyId(key).empty() || !ImGui::IsKeyPressed(key, false)) continue;

    // Local panels draw after Settings and their public key queries honor locks.
    // Keep Space locked even if its modifier is released before Space itself.
    ImGui::SetKeyOwner(key, ImGuiKeyOwner_Any, ImGuiInputFlags_LockUntilRelease);
    if (key == ImGuiKey_Escape) {
      close = true;
      break;
    }

    const ImGuiKeyChord chord = key | ImGui::GetIO().KeyMods;
    auto bindings = effectiveKeyboardBindings(app);
    window.shortcutFeedback = shortcutAssignmentError(bindings, chord, window.shortcutBeingChanged);
    if (window.shortcutFeedback.empty()) {
      const auto previous = std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding) {
        return binding.trigger == window.shortcutBeingChanged;
      });
      if (previous == bindings.end()) bindings.push_back({chord, {window.shortcutCommand}});
      else previous->trigger = chord;
      app.keyboardBindings = std::move(bindings);
      close = true;
    }
    break;
  }

  if (!window.shortcutFeedback.empty()) ImGui::TextUnformatted(window.shortcutFeedback.c_str());
  if (ImGui::Button("Cancel")) close = true;
  if (close) {
    window.shortcutFeedback.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  return true;
}

void removeAssignment(App &app, ImGuiKeyChord chord) {
  auto bindings = effectiveKeyboardBindings(app);
  bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [chord](const auto &binding) {
    return binding.trigger == chord;
  }), bindings.end());
  app.keyboardBindings = std::move(bindings);
  app.settingsWindow.shortcutFeedback.clear();
}

void restoreCommandDefaults(App &app, ApplicationCommand command, bool mac) {
  auto bindings = effectiveKeyboardBindings(app);
  bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [command](const auto &binding) {
    return binding.invokes(command);
  }), bindings.end());

  for (const auto &binding : defaultKeyboardBindings(mac)) {
    if (!binding.invokes(command)) continue;
    auto &feedback = app.settingsWindow.shortcutFeedback;
    feedback = shortcutAssignmentError(bindings, binding.trigger);
    if (!feedback.empty()) {
      feedback = shortcutLabel(binding.trigger, mac) + ". " + feedback;
      return;
    }
    bindings.push_back(binding);
  }
  app.keyboardBindings = std::move(bindings);
}
}  // namespace

void drawShortcutSettings(App &app) {
  if (!ImGui::CollapsingHeader("Keyboard Shortcuts")) return;

  auto &window = app.settingsWindow;
  const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
  if (ImGui::BeginCombo("Action", commandInfo(window.shortcutCommand).label)) {
    for (int index = 0; index < static_cast<int>(ApplicationCommand::Count); ++index) {
      const auto command = static_cast<ApplicationCommand>(index);
      if (ImGui::Selectable(commandInfo(command).label, window.shortcutCommand == command)) {
        window.shortcutCommand = command;
        window.shortcutFeedback.clear();
      }
    }
    ImGui::EndCombo();
  }

  ImGuiKeyChord remove = 0;
  bool assigned = false;
  for (const auto &binding : effectiveKeyboardBindings(app)) {
    if (!binding.invokes(window.shortcutCommand)) continue;
    assigned = true;
    ImGui::PushID(binding.trigger);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(shortcutLabel(binding.trigger, mac).c_str());
    ImGui::SameLine();
    const bool change = ImGui::Button("Change...");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) remove = binding.trigger;
    ImGui::PopID();
    if (change) beginCapture(app, binding.trigger);
  }
  if (remove) removeAssignment(app, remove);
  if (!assigned) ImGui::TextDisabled("Unassigned");

  if (ImGui::Button("Add shortcut...")) beginCapture(app);
  ImGui::SameLine();
  if (ImGui::Button("Restore action defaults")) restoreCommandDefaults(app, window.shortcutCommand, mac);
  if (ImGui::Button("Restore all defaults")) {
    app.keyboardBindings.reset();
    window.shortcutFeedback.clear();
  }
  if (!drawCapture(app) && !window.shortcutFeedback.empty())
    ImGui::TextWrapped("%s", window.shortcutFeedback.c_str());
}
}  // namespace ui
