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
  SelectPosition, SelectNext, SelectPrevious, SelectProcessor, TargetParameter,
  SetValue, AdjustValue, Increase, Decrease, ResetParameter, ToggleNode,
  NextPhoto, PreviousPhoto, MarkPick, MarkReject, MarkUnmarked, Count
};

struct ApplicationCommandInfo {
  const char *id;
  const char *label;
};

inline constexpr ApplicationCommandInfo applicationCommands[] = {
    {"open-image", "Open image"},
    {"open-folder", "Open Folder"},
    {"export", "Export"},
    {"undo", "Undo"},
    {"redo", "Redo"},
    {"copy-node", "Copy Node"},
    {"copy-full-grade", "Copy Full Grade"},
    {"paste", "Paste"},
    {"toggle-inspector", "Inspector"},
    {"toggle-filmstrip", "Filmstrip"},
    {"select-position", "Select Node by List Position"},
    {"select-next", "Select Next Node"},
    {"select-previous", "Select Previous Node"},
    {"select-processor", "Select Processor"},
    {"target-parameter", "Target Parameter"},
    {"set-value", "Set Value"},
    {"adjust-value", "Adjust by Amount"},
    {"increase", "Increase by Automatic Step"},
    {"decrease", "Decrease by Automatic Step"},
    {"reset-parameter", "Reset Parameter"},
    {"toggle-node", "Toggle Node"},
    {"next-photo", "Next Photograph"},
    {"previous-photo", "Previous Photograph"},
    {"mark-pick", "Mark as Pick"},
    {"mark-reject", "Mark as Reject"},
    {"mark-unmarked", "Mark as Unmarked"},
};

inline const ApplicationCommandInfo &commandInfo(ApplicationCommand command) {
  return applicationCommands[static_cast<int>(command)];
}

struct ProcessorIdentity {
  std::string backend;
  std::string identifier;
};

struct CommandStep {
  CommandStep(ApplicationCommand command);

  ApplicationCommand kind;
  std::variant<std::monostate, int, ProcessorIdentity, std::string, double> operand;
};

using CommandSequence = std::vector<CommandStep>;
std::string sequenceError(const CommandSequence &commands);

struct KeyboardBinding {
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
std::string shortcutAssignmentError(const KeyboardBindings &bindings, ImGuiKeyChord chord,
                                    ImGuiKeyChord replacing = 0);

}  // namespace ui
