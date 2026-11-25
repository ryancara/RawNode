#pragma once

#include "imgui.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

enum class ApplicationCommand {
  OpenImage, OpenFolder, Export, Undo, Redo, CopyNode, CopyFullGrade, Paste, Inspector, Filmstrip, Count
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
};

inline const ApplicationCommandInfo &commandInfo(ApplicationCommand command) {
  return applicationCommands[static_cast<int>(command)];
}

struct KeyboardBinding {
  ImGuiKeyChord trigger;
  std::vector<ApplicationCommand> commands;

  bool invokes(ApplicationCommand command) const {
    return commands.size() == 1 && commands.front() == command;
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
