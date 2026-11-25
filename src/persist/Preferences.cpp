#include "persist/Preferences.h"
#include "persist/PreferencesPriv.h"
#include "persist/Json.h"
#include "AppState.h"
#include "ui/Themes.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "platform/DirectoryDiscovery.h"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {
using persist_json::Type;
using persist_json::Value;

void warn(std::string &warning, const std::string &text) {
  if (!warning.empty()) warning += "\n";
  warning += text;
}
const Value *optional(const Value &root, const char *key) {
  const auto found = root.object.find(key);
  return found == root.object.end() ? nullptr : &found->second;
}
void integer(const Value &root, const char *key, int &out, int low, int high, std::string &warning) {
  if (const auto *value = optional(root, key)) {
    int number;
    if (persist_json::integer(*value, number) && number >= low && number <= high) out = number;
    else warn(warning, std::string(key) + ": invalid value; using default");
  }
}
void boolean(const Value &root, const char *key, bool &out, std::string &warning) {
  if (const auto *value = optional(root, key)) {
    if (value->type == Type::Boolean) out = value->raw == "true";
    else warn(warning, std::string(key) + ": expected boolean; using default");
  }
}
void size(const Value &root, const char *key, float &out, double low, double high, std::string &warning) {
  if (const auto *value = optional(root, key)) {
    double number;
    if (persist_json::finiteNumber(*value, number) && number >= low && number <= high) out = float(number);
    else warn(warning, std::string(key) + ": invalid size; using default");
  }
}
bool isValidUtf8Path(const std::string &path) {
  // Native filenames can contain byte sequences the strict preferences file cannot store.
  Value value;
  std::string error;
  return persist_json::read(jsonStringValue(path), value, error);
}
fs::path absoluteDirectoryPath(const std::string &path) {
  if (path.find('\0') != std::string::npos) return {};
  const auto nativePath = fs::u8path(path);
  return nativePath.is_absolute() ? nativePath : fs::path{};
}
void directory(const Value &root, const char *key, std::string &out, std::string &warning) {
  const auto *value = optional(root, key);
  if (!value) return;
  if (value->type == Type::String && (value->string.empty() || !absoluteDirectoryPath(value->string).empty()))
    out = value->string;
  else
    warn(warning, std::string(key) + ": expected an absolute directory path or an empty string; leaving unset");
}
void color(const Value &root, const char *key, ColorEncoding &out, std::string &warning) {
  const auto *pair = optional(root, key);
  if (!pair) return;
  const auto *gamut = optional(*pair, "colorSpace"), *gamma = optional(*pair, "gamma");
  ColorEncoding encoding;
  if (pair->type == Type::Object && gamut && gamma && gamut->type == Type::String &&
      gamma->type == Type::String && colorEncodingFromIds(gamut->string, gamma->string, encoding)) {
    out = encoding;
    return;
  }
  warn(warning, std::string(key) + ": incomplete, malformed or unknown colour pair; using built-in default");
}
std::optional<ui::CommandStep> readCommand(const Value &value) {
  const Value *id = value.type == Type::Object ? optional(value, "id") : &value;
  if (!id || id->type != Type::String) return std::nullopt;
  const auto kind = ui::commandFromId(id->string);
  if (!kind) return std::nullopt;

  ui::CommandStep step(*kind);
  if (std::holds_alternative<std::monostate>(step.operand))
    return value.type == Type::String ? std::optional(step) : std::nullopt;
  if (value.type != Type::Object) return std::nullopt;
  if (auto *position = std::get_if<int>(&step.operand)) {
    const auto *field = optional(value, "position");
    if (!field || !persist_json::integer(*field, *position)) return std::nullopt;
  } else if (auto *identity = std::get_if<ui::ProcessorIdentity>(&step.operand)) {
    const auto *backend = optional(value, "backend"), *processor = optional(value, "processor");
    if (!backend || backend->type != Type::String || !processor || processor->type != Type::String)
      return std::nullopt;
    *identity = {backend->string, processor->string};
  } else if (auto *parameter = std::get_if<std::string>(&step.operand)) {
    const auto *field = optional(value, "parameter");
    if (!field || field->type != Type::String) return std::nullopt;
    *parameter = field->string;
  } else {
    const auto *field = optional(value, "value");
    if (!field || !persist_json::finiteNumber(*field, std::get<double>(step.operand))) return std::nullopt;
  }
  return step;
}

std::optional<ui::KeyboardBinding> readKeyboardBinding(const Value &value) {
  const auto *key = optional(value, "key");
  const auto *modifiers = optional(value, "modifiers");
  const auto *commands = optional(value, "commands");
  if (value.type != Type::Object || !key || key->type != Type::String ||
      !modifiers || modifiers->type != Type::Array || !commands || commands->type != Type::Array)
    return std::nullopt;

  const auto namedKey = ui::keyboardKeyFromId(key->string);
  if (namedKey == ImGuiKey_None) return std::nullopt;
  ImGuiKeyChord chord = namedKey;
  for (const auto &name : modifiers->array) {
    if (name.type != Type::String) return std::nullopt;
    const auto modifier = ui::shortcutModifierFromId(name.string);
    if (modifier == ImGuiKey_None || (chord & modifier)) return std::nullopt;
    chord |= modifier;
  }
  ui::KeyboardBinding binding{chord, {}};
  for (const auto &element : commands->array) {
    auto step = readCommand(element);
    if (!step) return std::nullopt;
    binding.commands.push_back(std::move(*step));
  }
  if (!ui::sequenceError(binding.commands).empty()) return std::nullopt;
  return binding;
}

void loadKeyboardBindings(const Value &root, Preferences &out, std::string &warning) {
  const auto *value = optional(root, "keyboardBindings");
  if (!value) return;
  if (value->type != Type::Array) {
    warn(warning, "keyboardBindings: expected an array, using defaults");
    return;
  }

  ui::KeyboardBindings bindings;
  for (size_t index = 0; index < value->array.size(); ++index) {
    auto binding = readKeyboardBinding(value->array[index]);
    const auto error = binding ? ui::shortcutAssignmentError(bindings, binding->trigger)
                               : "Expected a known key, modifiers and a valid command sequence.";
    if (error.empty()) bindings.push_back(std::move(*binding));
    else warn(warning, "keyboardBindings entry " + std::to_string(index + 1) + ": " + error);
  }
  if (value->array.empty() || !bindings.empty()) out.keyboardBindings = std::move(bindings);
}

void writeKeyboardBindings(std::ostream &out, const ui::KeyboardBindings &bindings) {
  out << ",\n  \"keyboardBindings\":[";
  const char *separator = "";
  for (const auto &binding : bindings) {
    out << separator << "\n    {\"key\":"
        << jsonStringValue(std::string(ui::keyboardKeyId(static_cast<ImGuiKey>(binding.trigger & ~ImGuiMod_Mask_))))
        << ",\"modifiers\":[";
    const char *modifierSeparator = "";
    for (const auto &modifier : ui::shortcutModifiers) {
      if (!(binding.trigger & modifier.flag)) continue;
      out << modifierSeparator << jsonStringValue(modifier.id);
      modifierSeparator = ",";
    }
    out << "],\"commands\":[";
    const char *commandSeparator = "";
    for (const auto &step : binding.commands) {
      const auto id = jsonStringValue(ui::commandInfo(step.kind).id);
      out << commandSeparator;
      if (std::holds_alternative<std::monostate>(step.operand)) out << id;
      else {
        out << "{\"id\":" << id;
        if (const auto *position = std::get_if<int>(&step.operand)) out << ",\"position\":" << *position;
        else if (const auto *identity = std::get_if<ui::ProcessorIdentity>(&step.operand))
          out << ",\"backend\":" << jsonStringValue(identity->backend)
              << ",\"processor\":" << jsonStringValue(identity->identifier);
        else if (const auto *parameter = std::get_if<std::string>(&step.operand))
          out << ",\"parameter\":" << jsonStringValue(*parameter);
        else out << ",\"value\":" << std::setprecision(std::numeric_limits<double>::max_digits10)
                 << std::get<double>(step.operand);
        out << "}";
      }
      commandSeparator = ",";
    }
    out << "]}";
    separator = ",";
  }
  if (!bindings.empty()) out << "\n  ";
  out << "]";
}

std::string pairJson(ColorEncoding encoding) {
  return "{\"colorSpace\":" + jsonStringValue(rgbGamutId(encoding.gamut)) +
         ",\"gamma\":" + jsonStringValue(transferFunctionId(encoding.gamma)) + "}";
}
}  // namespace

bool validDefaultPresetPath(const std::string &path) {
  if (path.find('\0') != std::string::npos || !hasFilenameSuffix(path, ".rawnode.json")) return false;
  if (!isValidUtf8Path(path)) return false;
  const auto relative = fs::u8path(path);
  if (relative.has_root_path()) return false;
  for (const auto &component : relative)
    if (component.has_root_path() || component == "." || component == "..") return false;
  return true;
}

ConfigPaths preferences_detail::configPaths(const std::string &home, const std::string &configHome,
                                            std::string &warning) {
  warning.clear();
  fs::path directory;
#if defined(__APPLE__)
  if (fs::path(home).is_absolute()) directory = fs::path(home) / "Library/Application Support/RawNode";
#elif defined(_WIN32)
  (void)home;
  const auto appData = fs::u8path(configHome);
  if (appData.is_absolute()) directory = appData / "RawNode";
#else
  // XDG requires an absolute path. Relative values are ignored per its spec.
  if (fs::path(configHome).is_absolute()) directory = fs::path(configHome) / "RawNode";
  else if (fs::path(home).is_absolute()) directory = fs::path(home) / ".config/RawNode";
#endif
  if (directory.empty()) {
    warning = "Application configuration directory is unavailable; using in-memory settings.";
    return {};
  }
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (!ec && !fs::is_directory(directory, ec)) ec = std::make_error_code(std::errc::not_a_directory);
#ifndef _WIN32
  if (!ec && access(directory.c_str(), W_OK | X_OK) != 0)
    ec = std::make_error_code(std::errc::permission_denied);
#endif
  if (ec) {
    warning = directory.u8string() + ": " + ec.message() + "; using in-memory settings.";
    return {};
  }
  ConfigPaths paths{(directory / "preferences.json").u8string(), (directory / "layout.ini").u8string()};
  // ImGui has no error channel for ini writes. Reject an unusable destination
  // now and keep in-memory layout; never choose another directory.
  const auto status = fs::symlink_status(directory / "layout.ini", ec);
  if (ec == std::errc::no_such_file_or_directory) ec.clear();
  if (!ec && status.type() != fs::file_type::not_found) {
    if (!fs::is_regular_file(status)) ec = std::make_error_code(std::errc::invalid_argument);
#ifndef _WIN32
    else if (access(paths.layout.c_str(), R_OK | W_OK) != 0) ec = std::make_error_code(std::errc::permission_denied);
#endif
  }
  if (ec) {
    warning = paths.layout + ": " + ec.message() + "; using in-memory layout.";
    paths.layout.clear();
  }
  return paths;
}

ConfigPaths applicationConfigPaths(std::string &warning) {
#ifdef _WIN32
  const auto *appData = _wgetenv(L"APPDATA");
  return preferences_detail::configPaths("", appData ? fs::path(appData).u8string() : "", warning);
#else
  const auto env = [](const char *name) { const auto *value = std::getenv(name); return value ? value : ""; };
  return preferences_detail::configPaths(env("HOME"), env("XDG_CONFIG_HOME"), warning);
#endif
}

bool loadPreferences(const std::string &path, Preferences &out, std::string &warning) {
  out = Preferences{};
  warning.clear();
  if (path.empty()) return true;
  std::string json;
  const auto status = persist_detail::readGraphFileText(path, json, warning, persist_detail::FileFault::None);
  if (status == SidecarReadStatus::Absent) return true;
  if (status == SidecarReadStatus::Error) return false;
  Value root;
  if (!persist_json::read(json, root, warning)) { warning = path + ": " + warning; return false; }
  const auto *format = optional(root, "format"), *version = optional(root, "version");
  int number;
  if (root.type != Type::Object || !format || format->type != Type::String ||
      format->string != "rawnode-preferences" || !version ||
      !persist_json::integer(*version, number) || number != 1) {
    warning = path + ": unsupported preferences format/version; using defaults.";
    return false;
  }
  color(root, "rawWorking", out.rawWorking, warning);
  color(root, "output", out.output, warning);
  integer(root, "exportFormat", out.exportFormat, 0, 1, warning);
  integer(root, "jpegQuality", out.jpegQuality, 1, 100, warning);
  integer(root, "previewRes", out.previewRes, 0, kPreviewResCount - 1, warning);
  integer(root, "themeIndex", out.themeIndex, 0, themeCount() - 1, warning);
  boolean(root, "showRight", out.showRight, warning);
  boolean(root, "showNodes", out.showNodes, warning);
  if (const auto *value = optional(root, "nodesView")) {
    if (value->type == Type::String && value->string == "graph") out.nodesView = NodesView::Graph;
    else if (value->type != Type::String || value->string != "list")
      warn(warning, "nodesView: expected list or graph; using List View");
  }
  boolean(root, "showFilmstrip", out.showFilmstrip, warning);
  boolean(root, "showPresetPanel", out.showPresetPanel, warning);
  size(root, "rightW", out.rightW, 1, 10000, warning);
  size(root, "filmstripH", out.filmstripH, 48, 240, warning);
  directory(root, "presetDirectory", out.presetDirectory, warning);
  directory(root, "ctlScriptsDirectory", out.ctlScriptsDirectory, warning);
  if (const auto *value = optional(root, "defaultPreset")) {
    if (value->type != Type::String || (!value->string.empty() && !validDefaultPresetPath(value->string)))
      warn(warning, "defaultPreset: expected a preset path relative to the root directory; leaving unset");
    else if (!value->string.empty() && out.presetDirectory.empty())
      warn(warning, "defaultPreset: no root preset directory is configured; leaving unset");
    else
      out.defaultPreset = value->string;
  }
  loadKeyboardBindings(root, out, warning);
  if (!warning.empty()) warning = path + ":\n" + warning;
  return true;
}

bool preferences_detail::save(const std::string &path, const Preferences &p, std::string &error,
                              persist_detail::FileFault fault) {
  error.clear();
  if (path.empty()) { error = "Application configuration directory is unavailable"; return false; }
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\n  \"format\":\"rawnode-preferences\",\n  \"version\":1,\n"
      << "  \"rawWorking\":" << pairJson(p.rawWorking) << ",\n  \"output\":" << pairJson(p.output)
      << ",\n  \"exportFormat\":" << p.exportFormat << ",\n  \"jpegQuality\":" << p.jpegQuality
      << ",\n  \"previewRes\":" << p.previewRes << ",\n  \"themeIndex\":" << p.themeIndex
      << ",\n  \"showRight\":" << (p.showRight ? "true" : "false")
      << ",\n  \"showNodes\":" << (p.showNodes ? "true" : "false")
      << ",\n  \"nodesView\":\"" << (p.nodesView == NodesView::Graph ? "graph" : "list") << "\""
      << ",\n  \"showFilmstrip\":" << (p.showFilmstrip ? "true" : "false")
      << ",\n  \"showPresetPanel\":" << (p.showPresetPanel ? "true" : "false")
      << ",\n  \"rightW\":" << p.rightW
      << ",\n  \"filmstripH\":" << p.filmstripH;
  if (!p.presetDirectory.empty()) out << ",\n  \"presetDirectory\":" << jsonStringValue(p.presetDirectory);
  if (!p.defaultPreset.empty()) out << ",\n  \"defaultPreset\":" << jsonStringValue(p.defaultPreset);
  if (!p.ctlScriptsDirectory.empty()) out << ",\n  \"ctlScriptsDirectory\":" << jsonStringValue(p.ctlScriptsDirectory);
  if (p.keyboardBindings) writeKeyboardBindings(out, *p.keyboardBindings);
  out << "\n}\n";
  return persist_detail::replaceFileAtomically(path, out.str(), error, fault);
}

bool savePreferences(const std::string &path, const Preferences &preferences, std::string &error) {
  return preferences_detail::save(path, preferences, error, persist_detail::FileFault::None);
}

std::string directoryPreferenceError(const std::string &path) {
  if (path.empty()) return {};
  if (!isValidUtf8Path(path))
    return "The directory path must be valid UTF-8.";
  const auto nativePath = absoluteDirectoryPath(path);
  if (nativePath.empty()) return "Choose a valid absolute directory path.";
  std::error_code error;
  const auto status = fs::status(nativePath, error);
  if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found)
    return "The directory could not be found.";
  if (error) return "The directory is unavailable: " + error.message();
  if (!fs::is_directory(status)) return "The path is not a directory.";
  return {};
}

Preferences capturePreferences(const App &app) {
  Preferences p;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    p.rawWorking = app.rawWorkingEncoding;
    p.output = app.defaultOutputEncoding;
  }
  p.exportFormat = app.exportFormat; p.jpegQuality = app.jpegQuality;
  p.previewRes = app.previewRes; p.themeIndex = app.themeIndex;
  p.showNodes = app.showNodes; p.nodesView = app.nodesView;
  p.showRight = app.showRight; p.showFilmstrip = app.showFilmstrip;
  p.showPresetPanel = app.showPresetPanel;
  p.rightW = app.rightW; p.filmstripH = app.filmstripH;
  p.presetDirectory = app.presetDirectory;
  p.defaultPreset = app.defaultPreset;
  p.ctlScriptsDirectory = app.ctlScriptsDirectory;
  p.keyboardBindings = app.keyboardBindings;
  return p;
}

void applyPreferences(App &app, const Preferences &p) {
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.rawWorkingEncoding = p.rawWorking;
    app.defaultOutputEncoding = p.output;
    if (app.path.empty()) {
      app.photoRawWorkingEncoding = p.rawWorking;
      app.outputEncoding = p.output;
    }
  }
  app.exportFormat = p.exportFormat; app.jpegQuality = p.jpegQuality;
  app.previewRes = p.previewRes; app.themeIndex = p.themeIndex;
  app.showNodes = p.showNodes; app.nodesView = p.nodesView;
  app.showRight = p.showRight; app.showFilmstrip = p.showFilmstrip;
  app.showPresetPanel = p.showPresetPanel;
  app.rightW = p.rightW; app.filmstripH = p.filmstripH;
  app.presetDirectory = p.presetDirectory;
  app.defaultPreset = p.defaultPreset;
  app.ctlScriptsDirectory = p.ctlScriptsDirectory;
  app.keyboardBindings = p.keyboardBindings;
}

bool initialisePreferences(App &app, const ConfigPaths &paths) {
  Preferences preferences;
  std::string warning;
  const bool loaded = loadPreferences(paths.preferences, preferences, warning);
  applyPreferences(app, preferences);
  if (!warning.empty()) warn(app.preferencesWarning, warning);
  return loaded;
}
