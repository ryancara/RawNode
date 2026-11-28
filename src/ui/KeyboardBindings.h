#pragma once

#include "imgui.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ui {

enum class ApplicationCommand {
  // PR #135 actions through Filmstrip retain their standalone feedback.
  OpenImage, OpenFolder, Export, Undo, Redo, CopyNode, CopyFullGrade, Paste, Inspector, Filmstrip,
  SelectPosition, SelectNext, SelectPrevious, SelectProcessor, ToggleNode,
  SetValue, AdjustValue, Increase, Decrease, ResetParameter, AdjustWhileHeld,
  NextPhoto, PreviousPhoto, MarkPick, MarkReject, MarkUnmarked, Count
};

struct ApplicationCommandInfo {
  const char *id;
  const char *label;
  const char *category;
};

inline constexpr ApplicationCommandInfo applicationCommands[] = {
    {"open-image", "Open image", "File"},
    {"open-folder", "Open Folder", "File"},
    {"export", "Export", "File"},
    {"undo", "Undo", "Edit"},
    {"redo", "Redo", "Edit"},
    {"copy-node", "Copy Node", "Edit"},
    {"copy-full-grade", "Copy Full Grade", "Edit"},
    {"paste", "Paste", "Edit"},
    {"toggle-inspector", "Inspector", "View"},
    {"toggle-filmstrip", "Filmstrip", "View"},
    {"select-position", "Select Node by List Position", "Nodes"},
    {"select-next", "Select Next Node", "Nodes"},
    {"select-previous", "Select Previous Node", "Nodes"},
    {"select-processor", "Select Processor", "Nodes"},
    {"toggle-node", "Toggle Node", "Nodes"},
    {"set-value", "Set Value", "Parameters"},
    {"adjust-value", "Adjust by Amount", "Parameters"},
    {"increase", "Increase by Automatic Step", "Parameters"},
    {"decrease", "Decrease by Automatic Step", "Parameters"},
    {"reset-parameter", "Reset Parameter", "Parameters"},
    {"adjust-while-held", "Adjust While Held", "Parameters"},
    {"next-photo", "Next Photograph", "Photographs"},
    {"previous-photo", "Previous Photograph", "Photographs"},
    {"mark-pick", "Mark as Pick", "Photographs"},
    {"mark-reject", "Mark as Reject", "Photographs"},
    {"mark-unmarked", "Mark as Unmarked", "Photographs"},
};

inline const ApplicationCommandInfo &commandInfo(ApplicationCommand command) {
  return applicationCommands[static_cast<int>(command)];
}

struct ProcessorIdentity {
  std::string backend;
  std::string identifier;
};

struct ParameterAddress {
  ProcessorIdentity processor;
  std::string parameterId;
};

struct ScalarCommandOperand {
  ParameterAddress address;
  std::optional<double> value;
};

struct CommandStep {
  CommandStep(ApplicationCommand command);

  ApplicationCommand kind;
  std::variant<std::monostate, int, ProcessorIdentity, ScalarCommandOperand> operand;
};

using CommandSequence = std::vector<CommandStep>;
std::string sequenceError(const CommandSequence &commands);

struct KeyboardBinding {
  // Zero represents an unassigned key in the Settings draft.
  ImGuiKeyChord trigger;
  CommandSequence commands;

  bool invokes(ApplicationCommand command) const {
    return commands.size() == 1 && commands.front().kind == command;
  }
};

using KeyboardBindings = std::vector<KeyboardBinding>;

// In pinned ImGui, Ctrl means the primary modifier (Command on macOS).
// Super is the other modifier (physical Control on macOS).
struct ShortcutModifier {
  ImGuiKey flag;
  const char *id;
  const char *label;
  const char *macLabel;
};
inline constexpr ShortcutModifier shortcutModifiers[] = {
    {ImGuiMod_Ctrl, "primary", "Ctrl+", "⌘+"},
    {ImGuiMod_Shift, "shift", "Shift+", "Shift+"},
    {ImGuiMod_Alt, "alt", "Alt+", "Option+"},
    {ImGuiMod_Super, "secondary", "Super+", "Ctrl+"},
};

const KeyboardBindings &defaultKeyboardBindings(bool mac);
std::string_view keyboardKeyId(ImGuiKey key);
ImGuiKey keyboardKeyFromId(std::string_view id);
ImGuiKey shortcutModifierFromId(std::string_view id);
std::optional<ApplicationCommand> commandFromId(std::string_view id);
std::string shortcutLabel(ImGuiKeyChord chord, bool mac);
std::string shortcutLabels(const KeyboardBindings &bindings, ApplicationCommand command, bool mac);
// Empty means assignable. Skip the previous trigger while changing an assignment.
std::string shortcutAssignmentError(const KeyboardBindings &bindings, ImGuiKeyChord chord, bool held,
                                    ImGuiKeyChord replacing = 0);

}  // namespace ui
