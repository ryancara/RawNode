#include "ui/UiContext.h"
#include "ui/CommandTargets.h"
#include "NodeGraph.h"
#include "misc/cpp/imgui_stdlib.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>

namespace ui {
namespace {
using Command = ApplicationCommand;
constexpr const char *bindingPopup = "Edit keyboard binding";

std::string processorLabel(const ProcessorIdentity &identity, const Node *node) {
  return node ? nodeDisplayName(*node) + " (" + identity.backend + ")"
              : identity.backend + " / " + identity.identifier;
}

std::string parameterLabel(const ParameterAddress &address, const Node *node) {
  std::string label = address.parameterId;
  if (node && node->processor) {
    for (const auto &param : node->processor->parameters()) {
      if (param.id == address.parameterId && !param.label.empty()) {
        label = param.label;
        break;
      }
    }
  }
  return processorLabel(address.processor, node) + " / " + label;
}

std::string describeCommands(const App &app, const CommandSequence &commands) {
  std::string description;
  for (const auto &step : commands) {
    if (!description.empty()) description += " -> ";
    switch (step.kind) {
      case Command::SelectPosition: description += "Select node #"; break;
      case Command::SelectProcessor: description += "Select"; break;
      case Command::SetValue: description += "Set"; break;
      case Command::AdjustValue:
      case Command::AdjustWhileHeld: description += "Adjust"; break;
      case Command::Increase: description += "Increase"; break;
      case Command::Decrease: description += "Decrease"; break;
      case Command::ResetParameter: description += "Reset"; break;
      default: description += commandInfo(step.kind).label; break;
    }
    if (const auto *position = std::get_if<int>(&step.operand)) {
      description += std::to_string(*position);
    } else if (const auto *identity = std::get_if<ProcessorIdentity>(&step.operand)) {
      description += " " + processorLabel(*identity, resolveProcessor(app, *identity));
    } else if (const auto *scalar = std::get_if<ScalarCommandOperand>(&step.operand)) {
      description += " " + parameterLabel(scalar->address, resolveProcessor(app, scalar->address.processor));
      if (scalar->value) {
        char number[32];
        std::snprintf(number, sizeof number, "%.15g", *scalar->value);
        if (step.kind == Command::AdjustWhileHeld) description += ", step ";
        else description += step.kind == Command::SetValue ? " to " : " by ";
        description += number;
      }
      if (step.kind == Command::AdjustWhileHeld) description += " - Up/Down/Scroll";
    }
  }
  return description;
}

void beginBindingEditor(App &app, KeyboardBinding binding) {
  auto &window = app.settingsWindow;
  window.shortcutBeingEdited = binding.trigger;
  window.draftBinding = std::move(binding);
  window.recordingShortcut = false;
  window.shortcutFeedback.clear();
  ImGui::OpenPopup(bindingPopup);
}

void recordShortcut(App &app) {
  auto &window = app.settingsWindow;
  for (int index = ImGuiKey_Tab; index <= ImGuiKey_KeypadEqual; ++index) {
    const auto key = static_cast<ImGuiKey>(index);
    if (keyboardKeyId(key).empty() || !ImGui::IsKeyPressed(key, false)) continue;

    // Other panels query keys after Settings. In particular, keep Space owned
    // even if its modifier is released before Space itself.
    ImGui::SetKeyOwner(key, ImGuiKeyOwner_Any, ImGuiInputFlags_LockUntilRelease);
    if (key == ImGuiKey_Escape) {
      window.recordingShortcut = false;
      window.shortcutFeedback.clear();
    } else {
      const ImGuiKeyChord chord = key | ImGui::GetIO().KeyMods;
      window.shortcutFeedback = shortcutAssignmentError(effectiveKeyboardBindings(app), chord,
                                                        window.draftBinding.invokes(Command::AdjustWhileHeld),
                                                        window.shortcutBeingEdited);
      if (window.shortcutFeedback.empty()) {
        window.draftBinding.trigger = chord;
        window.recordingShortcut = false;
      }
    }
    return;
  }
}

bool commandCombo(Command &selected, bool standalone) {
  bool changed = false;
  if (ImGui::BeginCombo("Command", commandInfo(selected).label, ImGuiComboFlags_HeightLarge)) {
    std::string_view previousCategory;
    for (int index = 0; index < static_cast<int>(Command::Count); ++index) {
      const auto command = static_cast<Command>(index);
      const auto &info = commandInfo(command);
      if (previousCategory != info.category) {
        ImGui::SeparatorText(info.category);
        previousCategory = info.category;
      }
      const auto flags = command == Command::AdjustWhileHeld && !standalone
          ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None;
      if (ImGui::Selectable(info.label, selected == command, flags)) {
        selected = command;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

void drawProcessorFields(ProcessorIdentity &identity) {
  if (ImGui::BeginCombo("Backend", identity.backend.c_str())) {
    for (auto backend : {ProcessorBackend::Native, ProcessorBackend::OFX, ProcessorBackend::CTL}) {
      const char *id = processorBackendName(backend);
      if (ImGui::Selectable(id, identity.backend == id)) identity.backend = id;
    }
    ImGui::EndCombo();
  }
  ImGui::InputText("Processor ID", &identity.identifier);
}

void drawParameterPicker(const App &app, ParameterAddress &address) {
  const auto *resolved = resolveProcessor(app, address.processor);
  const auto preview = address.parameterId.empty() ? "Choose a parameter" : parameterLabel(address, resolved);
  if (ImGui::BeginCombo("Parameter in grade", preview.c_str())) {
    for (const auto *node : processorRows(app)) {
      if (!node->processor) continue;
      const auto identity = processorIdentity(*node);
      // A saved address can only reach the instance the command would resolve.
      if (resolveProcessor(app, identity) != node) continue;
      const auto name = processorLabel(identity, node);
      const auto parameters = node->processor->parameters();
      ImGui::PushID(node->id.value.c_str());
      for (const auto &param : parameters) {
        if (!editableScalar(param, parameters)) continue;
        const auto label = name + " / " + (param.label.empty() ? param.id : param.label);
        ImGui::PushID(param.id.c_str());
        if (ImGui::Selectable(label.c_str())) address = {identity, param.id};
        ImGui::PopID();
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  if (ImGui::CollapsingHeader("Manual identifiers")) {
    drawProcessorFields(address.processor);
    ImGui::InputText("Parameter ID", &address.parameterId);
  }
}

void drawOperand(const App &app, CommandStep &step) {
  if (auto *position = std::get_if<int>(&step.operand)) {
    ImGui::InputInt("List position", position);
  } else if (auto *identity = std::get_if<ProcessorIdentity>(&step.operand)) {
    const auto label = identity->identifier.empty() ? "Choose a processor"
        : processorLabel(*identity, resolveProcessor(app, *identity));
    if (ImGui::BeginCombo("Processor in grade", label.c_str())) {
      for (const auto *node : processorRows(app)) {
        ImGui::PushID(node->id.value.c_str());
        if (ImGui::Selectable(nodeDisplayName(*node).c_str())) *identity = processorIdentity(*node);
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if (ImGui::CollapsingHeader("Manual identifiers")) drawProcessorFields(*identity);
  } else if (auto *scalar = std::get_if<ScalarCommandOperand>(&step.operand)) {
    drawParameterPicker(app, scalar->address);
    if (step.kind == Command::AdjustWhileHeld) {
      bool automatic = !scalar->value;
      if (ImGui::Checkbox("Automatic step", &automatic)) {
        if (automatic) scalar->value.reset();
        else scalar->value = 1.0;
      }
      if (scalar->value) ImGui::InputDouble("Step", &*scalar->value, 0, 0, "%.15g");
      ImGui::TextWrapped("Hold the key in idle Preview. Press Up or Down, or scroll vertically over the image.");
    } else if (scalar->value) {
      ImGui::InputDouble(step.kind == Command::SetValue ? "Value" : "Signed amount", &*scalar->value, 0, 0, "%.15g");
    }
  }
}

void drawCommands(const App &app, CommandSequence &commands) {
  for (size_t i = 0; i < commands.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    auto &step = commands[i];
    auto kind = step.kind;
    if (commandCombo(kind, commands.size() == 1)) step = CommandStep(kind);
    drawOperand(app, step);
    bool rearranged = false;
    if (commands.size() > 1) {
      ImGui::BeginDisabled(i == 0);
      const bool up = ImGui::SmallButton("Up");
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(i + 1 == commands.size());
      const bool down = ImGui::SmallButton("Down");
      ImGui::EndDisabled();
      ImGui::SameLine();
      const bool remove = ImGui::SmallButton("Remove");
      if (up) std::swap(step, commands[i - 1]);
      if (down) std::swap(step, commands[i + 1]);
      if (remove) commands.erase(commands.begin() + i);
      rearranged = up || down || remove;
    }
    ImGui::PopID();
    ImGui::Separator();
    if (rearranged) break;
  }
  if ((commands.empty() || commands.front().kind != Command::AdjustWhileHeld) && ImGui::Button("Add command"))
    commands.emplace_back(Command::SelectNext);
}

void drawBindingEditor(App &app) {
  ImGui::SetNextWindowSize(ImVec2(500, 410), ImGuiCond_FirstUseEver);
  if (!ImGui::BeginPopupModal(bindingPopup)) return;

  auto &window = app.settingsWindow;
  auto &draft = window.draftBinding;
  if (window.recordingShortcut) recordShortcut(app);
  const auto label = draft.trigger ? shortcutLabel(draft.trigger, ImGui::GetIO().ConfigMacOSXBehaviors)
                                  : "No key recorded";
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label.c_str());
  ImGui::SameLine();
  ImGui::BeginDisabled(window.recordingShortcut);
  if (ImGui::Button("Record key")) window.recordingShortcut = true;
  ImGui::EndDisabled();

  ImGui::BeginDisabled(window.recordingShortcut);
  if (ImGui::BeginChild("Commands", ImVec2(0, -2 * ImGui::GetFrameHeightWithSpacing())))
    drawCommands(app, draft.commands);
  ImGui::EndChild();
  ImGui::EndDisabled();

  const auto &bindings = effectiveKeyboardBindings(app);
  const bool held = draft.invokes(Command::AdjustWhileHeld);
  std::string error = !draft.trigger ? "Record a key or key combination."
      : shortcutAssignmentError(bindings, draft.trigger, held, window.shortcutBeingEdited);
  if (draft.trigger && error.empty()) error = sequenceError(draft.commands);
  if (window.recordingShortcut)
    ImGui::TextWrapped("%s", window.shortcutFeedback.empty() ? "Press a key or chord. Escape stops recording."
                                                            : window.shortcutFeedback.c_str());
  else if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());

  ImGui::BeginDisabled(window.recordingShortcut || !error.empty());
  const bool save = ImGui::Button("Save");
  ImGui::EndDisabled();
  ImGui::SameLine();
  const bool cancel = ImGui::Button("Cancel");
  if (save) {
    auto updated = bindings;
    const auto previous = std::find_if(updated.begin(), updated.end(), [&](const auto &binding) {
      return binding.trigger == window.shortcutBeingEdited;
    });
    if (previous == updated.end()) updated.push_back(std::move(draft));
    else *previous = std::move(draft);
    app.keyboardBindings = std::move(updated);
  }
  if (save || cancel) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}
}  // namespace

void drawShortcutSettings(App &app) {
  if (!ImGui::CollapsingHeader("Keyboard Shortcuts")) return;

  const auto &bindings = effectiveKeyboardBindings(app);
  const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
  if (ImGui::Button("Add binding")) beginBindingEditor(app, {0, {Command::OpenImage}});
  ImGui::SameLine();
  const bool restore = ImGui::Button("Restore all defaults");

  const KeyboardBinding *edit = nullptr;
  ImGuiKeyChord remove = 0;
  if (bindings.empty()) ImGui::TextDisabled("No keyboard bindings");
  else if (ImGui::BeginTable("Bindings", 3, ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const auto &binding : bindings) {
      ImGui::PushID(binding.trigger);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      const auto input = shortcutLabel(binding.trigger, mac) +
                         (binding.invokes(Command::AdjustWhileHeld) ? " (held)" : "");
      ImGui::TextUnformatted(input.c_str());
      if (ImGui::TableNextColumn()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s", describeCommands(app, binding.commands).c_str());
      }
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Edit")) edit = &binding;
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) remove = binding.trigger;
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (edit) beginBindingEditor(app, *edit);
  if (restore) app.keyboardBindings.reset();
  else if (remove) {
    auto updated = bindings;
    updated.erase(std::find_if(updated.begin(), updated.end(), [remove](const auto &binding) {
      return binding.trigger == remove;
    }));
    app.keyboardBindings = std::move(updated);
  }
  drawBindingEditor(app);
}
}  // namespace ui
