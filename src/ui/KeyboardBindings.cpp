#include "ui/KeyboardBindings.h"

#include <iterator>

namespace ui {
namespace {
using Command = ApplicationCommand;

KeyboardBindings makeDefaults(bool mac) {
  KeyboardBindings bindings{
      {ImGuiMod_Ctrl | ImGuiKey_O, {Command::OpenImage}},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, {Command::OpenFolder}},
      {ImGuiMod_Ctrl | ImGuiKey_E, {Command::Export}},
      {ImGuiMod_Ctrl | ImGuiKey_Z, {Command::Undo}},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, {Command::Redo}},
      {ImGuiMod_Ctrl | ImGuiKey_C, {Command::CopyNode}},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C, {Command::CopyFullGrade}},
      {ImGuiMod_Ctrl | ImGuiKey_V, {Command::Paste}},
      {ImGuiMod_Ctrl | ImGuiKey_RightBracket, {Command::Inspector}},
      {ImGuiMod_Super | ImGuiKey_RightBracket, {Command::Inspector}},
      {ImGuiMod_Ctrl | ImGuiKey_Backslash, {Command::Filmstrip}},
      {ImGuiMod_Super | ImGuiKey_Backslash, {Command::Filmstrip}},
  };
  if (!mac) bindings.insert(bindings.begin() + 5, {ImGuiMod_Ctrl | ImGuiKey_Y, {Command::Redo}});
  return bindings;
}

// RawNode's saved names are independent of ImGui's debug strings and enum values.
constexpr struct {
  ImGuiKey key;
  const char *id;
} keys[] = {
#define KEY(name) {ImGuiKey_##name, #name}
    KEY(Tab), KEY(LeftArrow), KEY(RightArrow), KEY(UpArrow), KEY(DownArrow),
    KEY(PageUp), KEY(PageDown), KEY(Home), KEY(End), KEY(Insert), KEY(Delete),
    KEY(Backspace), KEY(Space), KEY(Enter), KEY(Escape), KEY(Menu),
    KEY(0), KEY(1), KEY(2), KEY(3), KEY(4), KEY(5), KEY(6), KEY(7), KEY(8), KEY(9),
    KEY(A), KEY(B), KEY(C), KEY(D), KEY(E), KEY(F), KEY(G), KEY(H), KEY(I), KEY(J),
    KEY(K), KEY(L), KEY(M), KEY(N), KEY(O), KEY(P), KEY(Q), KEY(R), KEY(S), KEY(T),
    KEY(U), KEY(V), KEY(W), KEY(X), KEY(Y), KEY(Z),
    KEY(F1), KEY(F2), KEY(F3), KEY(F4), KEY(F5), KEY(F6), KEY(F7), KEY(F8),
    KEY(F9), KEY(F10), KEY(F11), KEY(F12), KEY(F13), KEY(F14), KEY(F15), KEY(F16),
    KEY(F17), KEY(F18), KEY(F19), KEY(F20), KEY(F21), KEY(F22), KEY(F23), KEY(F24),
    KEY(Apostrophe), KEY(Comma), KEY(Minus), KEY(Period), KEY(Slash), KEY(Semicolon),
    KEY(Equal), KEY(LeftBracket), KEY(Backslash), KEY(RightBracket), KEY(GraveAccent),
    KEY(Keypad0), KEY(Keypad1), KEY(Keypad2), KEY(Keypad3), KEY(Keypad4),
    KEY(Keypad5), KEY(Keypad6), KEY(Keypad7), KEY(Keypad8), KEY(Keypad9),
    KEY(KeypadDecimal), KEY(KeypadDivide), KEY(KeypadMultiply), KEY(KeypadSubtract),
    KEY(KeypadAdd), KEY(KeypadEnter), KEY(KeypadEqual),
#undef KEY
};

const char *reservedShortcut(ImGuiKeyChord chord, bool mac) {
  const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
  const auto mods = chord & ImGuiMod_Mask_;

  if (key == ImGuiKey_Tab || key == ImGuiKey_Escape ||
      key == ImGuiKey_Enter || key == ImGuiKey_KeypadEnter)
    return "Reserved for keyboard navigation.";
  if (chord == (ImGuiMod_Ctrl | ImGuiKey_A)) return "Reserved for Filmstrip Select All.";
  if (chord == (ImGuiMod_Ctrl | ImGuiKey_F) || chord == (ImGuiMod_Super | ImGuiKey_F))
    return "Reserved for Inspector parameter search.";
  if (mods == 0 && (key == ImGuiKey_Delete || key == ImGuiKey_Backspace))
    return "Reserved for removing nodes or moving photographs to Trash.";
  if (mods == 0 || mods == ImGuiMod_Shift) {
    if (key == ImGuiKey_F) return "Reserved for framing the Graph.";
    if (key == ImGuiKey_Space) return "Reserved for panning the Preview.";
    if (key >= ImGuiKey_LeftArrow && key <= ImGuiKey_End) return "Reserved for keyboard navigation.";
  }

  if (mac) {
    if (mods == ImGuiMod_Ctrl && (key == ImGuiKey_Q || key == ImGuiKey_H ||
        key == ImGuiKey_M || key == ImGuiKey_Space || key == ImGuiKey_GraveAccent))
      return "Reserved by macOS.";
    if (chord == (ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_H) ||
        chord == (ImGuiMod_Ctrl | ImGuiMod_Super | ImGuiKey_F) ||
        chord == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_GraveAccent))
      return "Reserved by macOS.";
  } else if (chord == (ImGuiMod_Alt | ImGuiKey_F4)) {
    return "Reserved for closing the window.";
  }
  return nullptr;
}
}  // namespace

static_assert(std::size(applicationCommands) == static_cast<int>(ApplicationCommand::Count));

const KeyboardBindings &defaultKeyboardBindings(bool mac) {
  static const auto macBindings = makeDefaults(true);
  static const auto otherBindings = makeDefaults(false);
  return mac ? macBindings : otherBindings;
}

std::string_view keyboardKeyId(ImGuiKey key) {
  for (const auto &entry : keys)
    if (entry.key == key) return entry.id;
  return {};
}

ImGuiKey keyboardKeyFromId(std::string_view id) {
  for (const auto &entry : keys)
    if (entry.id == id) return entry.key;
  return ImGuiKey_None;
}

std::string shortcutLabel(ImGuiKeyChord chord, bool mac) {
  std::string label;
  for (const auto &modifier : shortcutModifiers)
    if (chord & modifier.flag) label += mac ? modifier.macLabel : modifier.label;

  switch (chord & ~ImGuiMod_Mask_) {
    case ImGuiKey_Apostrophe: return label + "'";
    case ImGuiKey_Comma: return label + ",";
    case ImGuiKey_Minus: return label + "-";
    case ImGuiKey_Period: return label + ".";
    case ImGuiKey_Slash: return label + "/";
    case ImGuiKey_Semicolon: return label + ";";
    case ImGuiKey_Equal: return label + "=";
    case ImGuiKey_LeftBracket: return label + "[";
    case ImGuiKey_RightBracket: return label + "]";
    case ImGuiKey_Backslash: return label + "\\";
    case ImGuiKey_GraveAccent: return label + "`";
    default: return label + ImGui::GetKeyName(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_));
  }
}

std::string shortcutLabels(const KeyboardBindings &bindings, ApplicationCommand command, bool mac) {
  std::string label;
  for (const auto &binding : bindings) {
    if (!binding.invokes(command)) continue;
    if (!label.empty()) label += " / ";
    label += shortcutLabel(binding.trigger, mac);
  }
  return label;
}

std::string shortcutAssignmentError(const KeyboardBindings &bindings, ImGuiKeyChord chord, bool mac,
                                    ImGuiKeyChord replacing) {
  if (keyboardKeyId(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_)).empty())
    return "This key cannot be assigned.";
  if (const char *reserved = reservedShortcut(chord, mac)) return reserved;
  for (const auto &binding : bindings) {
    if (binding.trigger != replacing && binding.trigger == chord) {
      if (binding.commands.size() == 1)
        return std::string("Already assigned to ") + commandInfo(binding.commands.front()).label + ".";
      return "This shortcut is already assigned.";
    }
  }
  return {};
}

}  // namespace ui
