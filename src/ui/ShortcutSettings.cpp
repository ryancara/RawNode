#include "ui/UiContext.h"
#include "ui/CommandTargets.h"
#include "NodeGraph.h"
#include "misc/cpp/imgui_stdlib.h"

#include "imgui_internal.h"

#include <algorithm>

namespace ui {
namespace {
constexpr const char *capturePopup = "Assign keyboard shortcut";

void beginCapture(App &app, ImGuiKeyChord replacing) {
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
      if (previous == bindings.end()) bindings.push_back({chord, window.draftCommands});
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

bool commandCombo(const char *label, ApplicationCommand &selected) {
  bool changed = false;
  if (ImGui::BeginCombo(label, commandInfo(selected).label)) {
    for (int index = 0; index < static_cast<int>(ApplicationCommand::Count); ++index) {
      const auto command = static_cast<ApplicationCommand>(index);
      if (ImGui::Selectable(commandInfo(command).label, selected == command)) {
        selected = command;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

void drawOperand(const App &app, CommandStep &step) {
  if (auto *position = std::get_if<int>(&step.operand)) {
    ImGui::InputInt("List position", position);
  } else if (auto *value = std::get_if<double>(&step.operand)) {
    ImGui::InputDouble(step.kind == ApplicationCommand::SetValue ? "Value" : "Signed amount", value, 0, 0, "%.15g");
  } else if (auto *identity = std::get_if<ProcessorIdentity>(&step.operand)) {
    const auto *live = resolveProcessor(app, *identity);
    const auto label = live ? nodeDisplayName(*live) : identity->identifier;
    if (ImGui::BeginCombo("Processor in grade", label.c_str())) {
      for (const auto *node : processorRows(app)) {
        ImGui::PushID(node->id.value.c_str());
        if (ImGui::Selectable(nodeDisplayName(*node).c_str()))
          *identity = processorIdentity(*node);
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::InputText("Backend", &identity->backend);
    ImGui::InputText("Processor ID", &identity->identifier);
  } else if (auto *parameter = std::get_if<std::string>(&step.operand)) {
    if (ImGui::BeginCombo("Parameter in grade", parameter->c_str())) {
      for (const auto *node : processorRows(app)) {
        if (!node->processor) continue;
        ImGui::PushID(node->id.value.c_str());
        const auto parameters = node->processor->parameters();
        for (const auto &param : parameters) {
          if (!editableScalar(param, parameters)) continue;
          const auto label = nodeDisplayName(*node) + " / " + (param.label.empty() ? param.id : param.label);
          ImGui::PushID(param.id.c_str());
          if (ImGui::Selectable(label.c_str())) *parameter = param.id;
          ImGui::PopID();
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::InputText("Parameter ID", parameter);
  }
}

constexpr const char *sequencePopup = "Edit command sequence";

void beginSequence(App &app, ImGuiKeyChord trigger, CommandSequence commands) {
  auto &window = app.settingsWindow;
  window.shortcutBeingChanged = trigger;
  window.draftCommands = std::move(commands);
  window.shortcutFeedback.clear();
  ImGui::OpenPopup(sequencePopup);
}

void drawSequenceEditor(App &app) {
  auto &window = app.settingsWindow;
  auto &commands = window.draftCommands;
  ImGui::SetNextWindowSize(ImVec2(500, 410), ImGuiCond_FirstUseEver);
  if (!ImGui::BeginPopupModal(sequencePopup)) return;
  if (ImGui::BeginChild("Commands", ImVec2(0, -2 * ImGui::GetFrameHeightWithSpacing()))) {
    for (size_t i = 0; i < commands.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      auto &step = commands[i];
      auto kind = step.kind;
      if (commandCombo("Command", kind)) step = CommandStep(kind);
      drawOperand(app, step);
      ImGui::BeginDisabled(i == 0);
      const bool up = ImGui::SmallButton("Up");
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(i + 1 == commands.size());
      const bool down = ImGui::SmallButton("Down");
      ImGui::EndDisabled();
      ImGui::SameLine();
      const bool remove = ImGui::SmallButton("Remove");
      ImGui::PopID();
      if (up) std::swap(step, commands[i - 1]);
      if (down) std::swap(step, commands[i + 1]);
      if (remove) commands.erase(commands.begin() + i);
      ImGui::Separator();
      if (up || down || remove) break;
    }
    if (ImGui::Button("Add command")) commands.emplace_back(ApplicationCommand::SelectCurrent);
  }
  ImGui::EndChild();
  const auto error = sequenceError(commands);
  ImGui::TextUnformatted(error.c_str());
  ImGui::BeginDisabled(!error.empty());
  const bool save = ImGui::Button(window.shortcutBeingChanged ? "Save commands" : "Assign shortcut...");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || save) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  if (!save) return;
  window.shortcutCommand = commands.front().kind;
  if (!window.shortcutBeingChanged) {
    beginCapture(app, 0);
    return;
  }
  auto bindings = effectiveKeyboardBindings(app);
  for (auto &binding : bindings)
    if (binding.trigger == window.shortcutBeingChanged) binding.commands = commands;
  app.keyboardBindings = std::move(bindings);
}
}  // namespace

void drawShortcutSettings(App &app) {
  if (!ImGui::CollapsingHeader("Keyboard Shortcuts")) return;

  auto &window = app.settingsWindow;
  const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
  if (commandCombo("Action", window.shortcutCommand)) window.shortcutFeedback.clear();

  ImGuiKeyChord remove = 0;
  bool assigned = false;
  for (const auto &binding : effectiveKeyboardBindings(app)) {
    if (binding.commands.front().kind != window.shortcutCommand) continue;
    assigned = true;
    ImGui::PushID(binding.trigger);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(shortcutLabel(binding.trigger, mac).c_str());
    ImGui::SameLine();
    const bool change = ImGui::Button("Change...");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) remove = binding.trigger;
    ImGui::SameLine();
    const bool edit = ImGui::Button("Edit commands...");
    if (binding.commands.size() > 1) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%zu commands)", binding.commands.size());
    }
    ImGui::PopID();
    if (edit) beginSequence(app, binding.trigger, binding.commands);
    if (change) beginCapture(app, binding.trigger);
  }
  if (remove) removeAssignment(app, remove);
  if (!assigned) ImGui::TextDisabled("Unassigned");

  if (ImGui::Button("Add shortcut...")) {
    window.draftCommands = {window.shortcutCommand};
    if (std::holds_alternative<std::monostate>(window.draftCommands.front().operand)) beginCapture(app, 0);
    else beginSequence(app, 0, window.draftCommands);
  }
  ImGui::SameLine();
  if (ImGui::Button("New sequence...")) beginSequence(app, 0, {window.shortcutCommand});
  const auto &defaults = defaultKeyboardBindings(mac);
  if (std::any_of(defaults.begin(), defaults.end(), [&](const auto &b) { return b.invokes(window.shortcutCommand); })) {
    if (ImGui::Button("Restore action defaults")) restoreCommandDefaults(app, window.shortcutCommand, mac);
    ImGui::SameLine();
  }
  if (ImGui::Button("Restore all defaults")) {
    app.keyboardBindings.reset();
    window.shortcutFeedback.clear();
  }
  drawSequenceEditor(app);
  if (!drawCapture(app) && !window.shortcutFeedback.empty())
    ImGui::TextWrapped("%s", window.shortcutFeedback.c_str());
}
}  // namespace ui
