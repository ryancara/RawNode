#include "PreferencesTests.h"
#include "AppState.h"
#include "CurrentDirectory.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/PreferencesPriv.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/ImGuiBackend.h"
#include "ui/UiContext.h"
#include "ui/DockLayout.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "preferences check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
using Fault = persist_detail::FileFault;
struct Files {
  fs::path dir = fs::temp_directory_path() /
      ("rawnode-preferences-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directory(dir); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
  std::string path(const std::string &name) const { return (dir / name).string(); }
};
void write(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary); out << text; out.close();
  if (!out) throw std::runtime_error("Cannot write fixture: " + path);
}
std::string bytes(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot read fixture: " + path);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::set<std::string> entries(const fs::path &dir) {
  std::set<std::string> result;
  for (const auto &entry : fs::directory_iterator(dir)) result.insert(entry.path().filename().string());
  return result;
}
const std::string header = R"("format":"rawnode-preferences","version":1)";
// Authored independently of the writer. Both pairs deliberately differ from
// built-in defaults, and from the separate saved-grade fixture below.
const std::string configured = "{" + header + R"(,
  "rawWorking":{"colorSpace":"aces-ap1","gamma":"rec709-camera"},
  "output":{"colorSpace":"display-p3","gamma":"linear"},
  "themeIndex":1,"exportFormat":0,"jpegQuality":73,"previewRes":0,
  "showRight":false,"showFilmstrip":false,
  "rightW":388.25,"filmstripH":144.5})";
const ColorEncoding storedRaw{RgbGamut::ACES_AP1, TransferFunction::Rec709};
const ColorEncoding storedOutput{RgbGamut::DisplayP3, TransferFunction::Linear};
const ColorEncoding fallbackRaw{RgbGamut::Rec2020, TransferFunction::Linear};
const ColorEncoding fallbackOutput{RgbGamut::Rec709, TransferFunction::SRGB};

bool codecAndOptionalFields() {
  Files files;
  const auto path = files.path("preferences.json");
  Preferences p; p.themeIndex = 0; p.rawWorking = storedRaw;
  std::string warning;
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.rawWorking == fallbackRaw && p.output == fallbackOutput && p.themeIndex == 2);
  App missing;
  CHECK(initialisePreferences(missing, {path, ""}) && missing.preferencesWarning.empty());
  CHECK(missing.rawWorkingEncoding == fallbackRaw && missing.defaultOutputEncoding == fallbackOutput);
  CHECK(missing.defaultPreset.empty());
  write(path, configured);
  App startup;
  CHECK(initialisePreferences(startup, {path, ""}) && startup.preferencesWarning.empty());
  CHECK(startup.rawWorkingEncoding == storedRaw && startup.defaultOutputEncoding == storedOutput);
  CHECK(startup.defaultPreset.empty());
  CHECK(startup.themeIndex == 1 && startup.previewRes == 0 && startup.exportFormat == 0 && startup.jpegQuality == 73);
  CHECK(!startup.showRight && !startup.showFilmstrip);
  CHECK(startup.rightW == 388.25f && startup.filmstripH == 144.5f);
  CHECK(savePreferences(path, capturePreferences(startup), warning) && warning.empty());
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.rawWorking == storedRaw && p.output == storedOutput && p.themeIndex == 1 && p.jpegQuality == 73);
  CHECK(p.rightW == 388.25f && p.filmstripH == 144.5f);
  CHECK(!p.showRight && !p.showFilmstrip && p.exportFormat == 0 && p.previewRes == 0);

  // A minimal file retains its valid theme and accepts future optional fields.
  write(path, "{" + header + R"(,"themeIndex":1,"future":{"key":[1,2]}})");
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.themeIndex == 1 && p.jpegQuality == 92 && p.previewRes == 1 && p.showRight && p.showFilmstrip);
  CHECK(p.rawWorking == fallbackRaw && p.output == fallbackOutput);
  for (const auto *field : {"rawWorking", "output"}) {
    const std::string other = std::string(field) == "rawWorking" ? "output" : "rawWorking";
    write(path, "{" + header + ",\"" + other +
          R"(":{"colorSpace":"display-p3","gamma":"linear"}})");
    CHECK(loadPreferences(path, p, warning) && warning.empty());
    CHECK((std::string(field) == "rawWorking" ? p.rawWorking : p.output) ==
          (std::string(field) == "rawWorking" ? fallbackRaw : fallbackOutput));
    CHECK((std::string(field) == "rawWorking" ? p.output : p.rawWorking) == storedOutput);
    for (const std::string bad : {"null", "7", "[]", R"({})", R"({"colorSpace":"rec709"})",
         R"({"gamma":"linear"})", R"({"colorSpace":false,"gamma":"linear"})",
         R"({"colorSpace":"unknown","gamma":"linear"})", R"({"colorSpace":"rec709","gamma":"unknown"})"}) {
      write(path, "{" + header + ",\"" + field + "\":" + bad + ",\"" + other +
            R"(":{"colorSpace":"display-p3","gamma":"linear"},"themeIndex":1})");
      CHECK(loadPreferences(path, p, warning) && !warning.empty() && p.themeIndex == 1);
      CHECK((std::string(field) == "rawWorking" ? p.rawWorking : p.output) ==
            (std::string(field) == "rawWorking" ? fallbackRaw : fallbackOutput));
      CHECK((std::string(field) == "rawWorking" ? p.output : p.rawWorking) == storedOutput);
    }
  }
  write(path, "{" + header + R"(,"jpegQuality":0,"filmstripH":1,"themeIndex":1})");
  CHECK(loadPreferences(path, p, warning) && !warning.empty());
  CHECK(p.jpegQuality == 92 && p.filmstripH == 96 && p.themeIndex == 1);
  write(path, R"({"format":"rawnode-preferences","version":9})");
  CHECK(!loadPreferences(path, p, warning) && !warning.empty());
  write(path, "{");
  CHECK(!loadPreferences(path, p, warning) && !warning.empty());
  return true;
}

bool defaultKeyboardBindings() {
  using Command = ui::ApplicationCommand;
  // Effective shortcuts from the pre-feature handlers at 7a5dd82.
  const ui::KeyboardBinding expected[]{
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
  for (const bool mac : {false, true}) {
    const auto &actual = ui::defaultKeyboardBindings(mac);
    CHECK(actual.size() == std::size(expected) + (mac ? 0 : 1));
    const auto matches = [&](ImGuiKeyChord trigger, Command command) {
      return std::count_if(actual.begin(), actual.end(), [&](const auto &binding) {
        return binding.trigger == trigger && binding.invokes(command);
      });
    };
    for (const auto &binding : expected) CHECK(matches(binding.trigger, binding.commands.front().kind) == 1);
    CHECK(matches(ImGuiMod_Ctrl | ImGuiKey_Y, Command::Redo) == (mac ? 0 : 1));
  }
  return true;
}

bool keyboardBindingsLifecycle() {
  Files files;
  const ConfigPaths config{files.path("preferences.json"), ""};
  write(config.preferences, configured);
  App first;
  CHECK(initialisePreferences(first, config) && !first.keyboardBindings);
  CHECK(prepareShutdown(first, config, true));
  CHECK(bytes(config.preferences).find("keyboardBindings") == std::string::npos);

  // Hand-authored stable IDs exercise the reader independently of its writer.
  write(config.preferences, "{" + header + R"(,"themeIndex":1,"keyboardBindings":[
    {"key":"K","modifiers":["primary","shift"],"commands":["undo"]},
    {"key":"K","modifiers":["shift","primary"],"commands":["redo"]},
    {"key":"Backspace","modifiers":[],"commands":["paste"]},
    {"key":625,"modifiers":[],"commands":["export"]},
    {"key":"P","modifiers":[],"commands":["export","redo"]},
    {"key":"PageDown","modifiers":[],"commands":["toggle-filmstrip"]},
    {"key":"Home","modifiers":[],"commands":["toggle-inspector"]},
    {"key":"Q","modifiers":["primary"],"commands":["copy-node"]},
    {"key":"F4","modifiers":["alt"],"commands":["paste"]}
  ]})");
  App loaded;
  CHECK(initialisePreferences(loaded, config) && loaded.themeIndex == 1);
  CHECK(loaded.preferencesWarning.find("Already assigned to Undo") != std::string::npos);
  CHECK(loaded.preferencesWarning.find("Reserved for removing nodes") != std::string::npos);
  CHECK(loaded.preferencesWarning.find("keyboardBindings entry 5") != std::string::npos);
  const auto checkBindings = [](const App &app) {
    using ui::ApplicationCommand;
    if (!app.keyboardBindings || app.keyboardBindings->size() != 5) return false;
    const auto &bindings = *app.keyboardBindings;
    return bindings[0].trigger == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K) &&
           bindings[0].invokes(ApplicationCommand::Undo) &&
           bindings[1].trigger == ImGuiKey_PageDown && bindings[1].invokes(ApplicationCommand::Filmstrip) &&
           bindings[2].trigger == ImGuiKey_Home && bindings[2].invokes(ApplicationCommand::Inspector) &&
           bindings[3].trigger == (ImGuiMod_Ctrl | ImGuiKey_Q) && bindings[3].invokes(ApplicationCommand::CopyNode) &&
           bindings[4].trigger == (ImGuiMod_Alt | ImGuiKey_F4) && bindings[4].invokes(ApplicationCommand::Paste);
  };
  CHECK(checkBindings(loaded) && prepareShutdown(loaded, config, true));

  App restarted;
  CHECK(initialisePreferences(restarted, config) && restarted.preferencesWarning.empty());
  CHECK(checkBindings(restarted) && restarted.themeIndex == 1);
  restarted.keyboardBindings->clear();
  CHECK(prepareShutdown(restarted, config, true));
  App unbound;
  CHECK(initialisePreferences(unbound, config) && unbound.preferencesWarning.empty());
  CHECK(unbound.keyboardBindings && unbound.keyboardBindings->empty());

  write(config.preferences, "{" + header + R"(,"themeIndex":1,"keyboardBindings":"invalid"})");
  App malformed;
  CHECK(initialisePreferences(malformed, config) && !malformed.preferencesWarning.empty());
  CHECK(!malformed.keyboardBindings && malformed.themeIndex == 1);
  write(config.preferences, "{" + header + R"(,"themeIndex":1,"keyboardBindings":[
    {"key":"Tab","modifiers":[],"commands":["undo"]},
    {"key":"F8","modifiers":[],"commands":["unknown-command"]},
    {"key":"F8","modifiers":["unknown-modifier"],"commands":["undo"]}
  ]})");
  App invalidEntries;
  CHECK(initialisePreferences(invalidEntries, config) && !invalidEntries.preferencesWarning.empty());
  CHECK(!invalidEntries.keyboardBindings && invalidEntries.themeIndex == 1);
  return true;
}

bool typedBindingRoundTrip() {
  using Command = ui::ApplicationCommand;
  Files files;
  const auto path = files.path("commands.json");
  write(path, "{" + header + R"(,"keyboardBindings":[
    {"key":"F8","modifiers":[],"commands":[{"id":"select-position","position":2},
      {"id":"select-processor","backend":"ctl","processor":"/future/grade.ctl"},
      {"id":"target-parameter","parameter":"gain"},{"id":"adjust-value","value":-0.12345678901234567},
      "reset-parameter"]},
    {"key":"F9","modifiers":[],"commands":["toggle-inspector",{"id":"adjust-value","value":"1"}]},
    {"key":"F10","modifiers":[],"commands":[{"id":"select-position","position":0}]}
  ]})");
  Preferences preferences;
  std::string warning;
  CHECK(loadPreferences(path, preferences, warning) && !warning.empty());
  for (int read = 0; read < 2; ++read) {
    CHECK(preferences.keyboardBindings && preferences.keyboardBindings->size() == 1);
    const auto &binding = preferences.keyboardBindings->front();
    CHECK(binding.trigger == ImGuiKey_F8 && binding.commands.size() == 5);
    const auto &steps = binding.commands;
    CHECK(steps[0].kind == Command::SelectPosition && std::get<int>(steps[0].operand) == 2);
    const auto &identity = std::get<ui::ProcessorIdentity>(steps[1].operand);
    CHECK(steps[1].kind == Command::SelectProcessor && identity.backend == "ctl" && identity.identifier == "/future/grade.ctl");
    CHECK(steps[2].kind == Command::TargetParameter && std::get<std::string>(steps[2].operand) == "gain");
    CHECK(steps[3].kind == Command::AdjustValue && std::get<double>(steps[3].operand) == -0.12345678901234567);
    CHECK(steps[4].kind == Command::ResetParameter);
    if (read == 0) {
      CHECK(savePreferences(path, preferences, warning));
      CHECK(loadPreferences(path, preferences, warning) && warning.empty());
    }
  }
  return true;
}

bool nodesViewPreference() {
  Files files;
  const auto path = files.path("preferences.json");
  Preferences p; std::string warning;
  // Old preference files remain List by default, without a warning.
  write(path, "{" + header + R"(,"themeIndex":1})");
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.nodesView == NodesView::List && p.showNodes && p.themeIndex == 1);
  for (const auto mode : {std::string("list"), std::string("graph")}) {
    write(path, "{" + header + ",\"nodesView\":\"" + mode + R"(","showNodes":false})");
    App app;
    CHECK(initialisePreferences(app, {path, ""}) && app.preferencesWarning.empty());
    CHECK(app.nodesView == (mode == "graph" ? NodesView::Graph : NodesView::List) && !app.showNodes);
    CHECK(savePreferences(path, capturePreferences(app), warning));
    CHECK(loadPreferences(path, p, warning) && warning.empty());
    CHECK(p.nodesView == app.nodesView && !p.showNodes);
  }
  for (const auto *invalid : {"null", "true", "7", "[]", "{}", "\"GRAPH\"", "\"unknown\""}) {
    write(path, "{" + header + ",\"nodesView\":" + invalid + R"(,"themeIndex":1})");
    CHECK(loadPreferences(path, p, warning) && warning.find("nodesView") != std::string::npos);
    CHECK(p.nodesView == NodesView::List && p.themeIndex == 1);
  }
  return true;
}

bool directoryPreferencesLifecycle() {
  Files files;
  const ConfigPaths config{files.path("preferences.json"), ""};
  const auto presets = files.dir / fs::u8path(u8"pr\u00e9sets");
  const auto scripts = files.dir / "CTL scripts";
  CHECK(fs::create_directory(presets) && fs::create_directory(scripts));
  CHECK(directoryPreferenceError(presets.u8string()).empty());
  write(config.preferences, configured);

  App first;
  CHECK(initialisePreferences(first, config) && first.preferencesWarning.empty());
  CHECK(first.presetDirectory.empty() && first.ctlScriptsDirectory.empty());
  CHECK(!first.showPresetPanel);
  first.presetDirectory = presets.u8string();
  first.ctlScriptsDirectory = scripts.u8string();
  first.showPresetPanel = true;
  CHECK(prepareShutdown(first, config, true));

  App restarted;
  CHECK(initialisePreferences(restarted, config) && restarted.preferencesWarning.empty());
  CHECK(restarted.presetDirectory == presets.u8string() && restarted.ctlScriptsDirectory == scripts.u8string());
  CHECK(restarted.showPresetPanel);
  CHECK(restarted.rawWorkingEncoding == storedRaw && restarted.defaultOutputEncoding == storedOutput);
  CHECK(restarted.themeIndex == 1 && restarted.jpegQuality == 73);
  CHECK(!restarted.showRight && !restarted.showFilmstrip);

  restarted.presetDirectory.clear();
  restarted.ctlScriptsDirectory.clear();
  CHECK(prepareShutdown(restarted, config, true));
  const auto saved = bytes(config.preferences);
  CHECK(saved.find("presetDirectory") == std::string::npos && saved.find("ctlScriptsDirectory") == std::string::npos);
  Preferences p;
  std::string warning;
  CHECK(loadPreferences(config.preferences, p, warning) && warning.empty());
  CHECK(p.presetDirectory.empty() && p.ctlScriptsDirectory.empty());
  CHECK(p.themeIndex == 1 && p.jpegQuality == 73);
  return true;
}

bool invalidDirectoryPreferences() {
  Files files;
  const auto path = files.path("preferences.json");
  Preferences p;
  std::string warning;
  write(path, "{" + header + R"(,"presetDirectory":"","ctlScriptsDirectory":""})");
  CHECK(loadPreferences(path, p, warning) && warning.empty());
  CHECK(p.presetDirectory.empty() && p.ctlScriptsDirectory.empty());
  write(path, "{" + header + R"(,"presetDirectory":7,"ctlScriptsDirectory":"relative/path","themeIndex":1})");
  CHECK(loadPreferences(path, p, warning));
  CHECK(warning.find("presetDirectory") != std::string::npos &&
        warning.find("ctlScriptsDirectory") != std::string::npos);
  CHECK(p.presetDirectory.empty() && p.ctlScriptsDirectory.empty() && p.themeIndex == 1);

  // A missing drive or a moved folder must not erase the user's selection.
  p.presetDirectory = (files.dir / "missing").u8string();
  p.ctlScriptsDirectory = (files.dir / "preferences.json").u8string();
  CHECK(savePreferences(path, p, warning));
  Preferences loaded;
  CHECK(loadPreferences(path, loaded, warning) && warning.empty());
  CHECK(loaded.presetDirectory == p.presetDirectory && loaded.ctlScriptsDirectory == p.ctlScriptsDirectory);
  CHECK(!directoryPreferenceError(loaded.presetDirectory).empty());
  CHECK(!directoryPreferenceError(loaded.ctlScriptsDirectory).empty());
  CHECK(directoryPreferenceError(files.dir.u8string()).empty());
  CHECK(directoryPreferenceError("").empty() && !fs::exists(files.dir / "missing"));

  const auto invalidUtf8 = files.dir.u8string() + "/invalid-\xff";
#ifdef __linux__
  // Reproduce a real directory the picker can return on Linux.
  CHECK(fs::create_directory(fs::path(invalidUtf8)));
#endif
  CHECK(directoryPreferenceError(invalidUtf8).find("UTF-8") != std::string::npos);
  return true;
}

bool defaultPresetPreference() {
  Files files;
  const ConfigPaths config{files.path("preferences.json"), ""};
  const std::string relative = u8"Looks/caf\u00e9.RaWnOdE.JsOn";
  CHECK(validDefaultPresetPath(relative));
  CHECK(!validDefaultPresetPath("bad\xff.rawnode.json"));
  App first;
  first.presetDirectory = files.dir.u8string();
  first.defaultPreset = relative;
  CHECK(prepareShutdown(first, config, true));
  // Missing files are retained across startup, so opening can report the error
  // and the user can fix the existing selection.
  CHECK(!fs::exists(files.dir / fs::u8path(relative)));
  App restarted;
  CHECK(initialisePreferences(restarted, config) && restarted.preferencesWarning.empty());
  CHECK(restarted.presetDirectory == first.presetDirectory && restarted.defaultPreset == relative);
  restarted.defaultPreset.clear();
  CHECK(prepareShutdown(restarted, config, true));
  CHECK(bytes(config.preferences).find("defaultPreset") == std::string::npos);

  Preferences preferences;
  std::string warning;
  const auto root = ",\"presetDirectory\":" + jsonStringValue(files.dir.u8string());
  write(config.preferences, "{" + header + root + R"(,"defaultPreset":""})");
  CHECK(loadPreferences(config.preferences, preferences, warning) && warning.empty());
  CHECK(preferences.defaultPreset.empty());
  for (const auto &invalid : {std::string("7"), jsonStringValue(files.path("absolute.rawnode.json")),
       jsonStringValue("../outside.rawnode.json"), jsonStringValue("nested/../escape.rawnode.json"),
       jsonStringValue("wrong.json"), jsonStringValue(std::string("bad\0.rawnode.json", 17))}) {
    write(config.preferences, "{" + header + root + ",\"defaultPreset\":" + invalid + R"(,"themeIndex":1})");
    CHECK(loadPreferences(config.preferences, preferences, warning));
    CHECK(warning.find("defaultPreset") != std::string::npos && preferences.defaultPreset.empty());
    CHECK(preferences.presetDirectory == first.presetDirectory && preferences.themeIndex == 1);
  }
  write(config.preferences, "{" + header + R"(,"defaultPreset":"look.rawnode.json"})");
  CHECK(loadPreferences(config.preferences, preferences, warning));
  CHECK(warning.find("defaultPreset") != std::string::npos && preferences.defaultPreset.empty());
  return true;
}

bool atomicFailuresAndPaths() {
  Files files;
  const auto home = files.path("home"), xdg = files.path("config");
  std::string error;
  const auto config = preferences_detail::configPaths(home, xdg, error);
  CHECK(error.empty());
#if defined(__APPLE__)
  const auto expected = fs::path(home) / "Library/Application Support/RawNode";
#else
  const auto expected = fs::path(xdg) / "RawNode";
#endif
  CHECK(config.preferences == (expected / "preferences.json").string());
  CHECK(config.layout == (expected / "layout.ini").string());
  const auto fallback = preferences_detail::configPaths(home, "", error);
#ifndef _WIN32
  CHECK(fallback.preferences == (fs::path(home) /
#ifdef __APPLE__
      "Library/Application Support/RawNode/preferences.json"
#else
      ".config/RawNode/preferences.json"
#endif
      ).string());
#endif
  const auto unresolved = preferences_detail::configPaths("", "", error);
  CHECK(unresolved.preferences.empty() && unresolved.layout.empty() && !error.empty());
  write(files.path("blocker"), "not a directory");
  const auto blocked = preferences_detail::configPaths(files.path("blocker/child"), files.path("blocker/child"), error);
  CHECK(blocked.preferences.empty() && blocked.layout.empty() && !error.empty());
  write(config.preferences, configured);
  const auto original = bytes(config.preferences);
  Preferences p;
  for (const auto fault : {Fault::PermissionStat, Fault::PermissionCheck, Fault::TempCreate,
       Fault::Write, Fault::Flush,
#ifndef _WIN32
       Fault::PermissionCopy,
#endif
       Fault::Close, Fault::Replace}) {
    CHECK(!preferences_detail::save(config.preferences, p, error, fault) && !error.empty());
    CHECK(bytes(config.preferences) == original);
    CHECK(entries(expected) == std::set<std::string>{"preferences.json"});
  }
  // Each invalid destination must disable layout persistence, without changing
  // the preferences path or falling back to CWD/photo folders.
  const auto layoutPath = expected / "layout.ini";
  const auto disabledLayout = [&] {
    const auto invalid = preferences_detail::configPaths(home, xdg, error);
    return invalid.preferences == config.preferences && invalid.layout.empty() && !error.empty();
  };
  fs::create_directory(layoutPath);
  CHECK(disabledLayout());
  fs::remove(layoutPath);
#ifndef _WIN32
  // A valid regular-file target distinguishes lstat from accidentally following
  // the link. A dangling link must be refused as well.
  write(files.path("layout-target.ini"), "[Window][Target]\nPos=1,2\n");
  fs::create_symlink(files.path("layout-target.ini"), layoutPath);
  CHECK(disabledLayout());
  CHECK(bytes(files.path("layout-target.ini")) == "[Window][Target]\nPos=1,2\n");
  fs::remove(layoutPath);
  fs::create_symlink(files.path("missing-layout-target.ini"), layoutPath);
  CHECK(disabledLayout());
  fs::remove(layoutPath);
  // FIFO is deterministically unusable as a native ini file, independent of
  // the test user's effective permissions.
  CHECK(mkfifo(layoutPath.c_str(), 0600) == 0);
  CHECK(disabledLayout());
  fs::remove(layoutPath);
#endif
  CHECK(!savePreferences("", p, error) && !error.empty());
  CHECK(!savePreferences(files.path("missing/preferences.json"), p, error));
#ifndef _WIN32
  fs::create_symlink(config.preferences, files.path("linked.json"));
  CHECK(!savePreferences(files.path("linked.json"), p, error) && bytes(config.preferences) == original);
#endif
  return true;
}

bool protectedPreferencesShutdown() {
  Files files;
  const ConfigPaths config{files.path("preferences.json"), files.path("layout.ini")};
  // Future version and unknown setting are authored independently of the writer.
  // Malformed JSON uses the same protected startup/shutdown behaviour.
  for (const std::string original : {
       R"({"format":"rawnode-preferences","version":2,"futureShortcutMap":{"open":"Q"}})",
       "{ malformed preferences"}) {
    write(config.preferences, original);
    App app;
    const bool preferencesLoaded = initialisePreferences(app, config);
    app.themeIndex = 1;
    app.presetDirectory = app.ctlScriptsDirectory = files.dir.u8string();
    CHECK(prepareShutdown(app, config, preferencesLoaded));
    CHECK(bytes(config.preferences) == original);
    CHECK(!preferencesLoaded && !app.preferencesWarning.empty());
  }
  return true;
}

bool decodeRaw(const std::string &, Image &image, ColorEncoding &detected, bool &raw, ColorEncoding requested) {
  image.w = image.h = 2; image.px.assign(16, 0.25f);
  detected = requested; raw = true;
  return true;
}
std::string newPhoto(const fs::path &dir, const char *name) {
  const auto path = (dir / name).string(); write(path, "synthetic RAW"); return path;
}
bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto state) {
    return !state.previewBusy && !state.previewPending && !state.displayPending;
  });
}

bool startupPhotoFolderAndShutdown() {
  Files files;
  fs::create_directory(files.path("cwd")); fs::create_directory(files.path("a")); fs::create_directory(files.path("b"));
  CurrentDirectory cwd(files.path("cwd"));
  std::string warning;
  const auto config = preferences_detail::configPaths(files.path("home"), files.path("config"), warning);
  CHECK(warning.empty());
  write(config.preferences, configured);
  const auto a = newPhoto(files.path("a"), "a.nef"), b = newPhoto(files.path("b"), "b.nef");
  // Retired files are deliberately plausible but contain conflicting choices.
  const std::string legacy = R"({"format":"ofxrawhost-workspace","version":1,"activeImage":"missing.nef","gui":{"themeIndex":0,"showLeft":true,"rawDefaultColorSpace":"rec709","rawDefaultGamma":"linear"}})";
  write(files.path("a/workspace.ofxrawhost.json"), legacy);
  write(files.path("a/.ofxrawhost-layout.ini"), "legacy layout A\n");
  write(files.path("cwd/ofxrawhost.ini"), "legacy CWD layout\n");
  App app;
  CHECK(initialisePreferences(app, config));
  document_detail::ActionTestOptions options; options.decode = decodeRaw;
  CHECK(document_detail::openFolder(app, files.path("a"), {}, options));
  CHECK(fs::equivalent(app.path, a));
  // Critical startup oracle: decoded pixels and new persisted grade consume the
  // hand-authored pairs, so merely reading/storing them cannot pass this test.
  CHECK(app.inputEncoding == storedRaw && app.photoRawWorkingEncoding == storedRaw && app.outputEncoding == storedOutput);
  PersistFile grade; std::string error;
  CHECK(readGraphSidecar(inputSidecarPath(a), grade, error) == SidecarReadStatus::Loaded && grade.grade);
  CHECK(grade.grade->color.rawColorSpace == "aces-ap1" && grade.grade->color.rawGamma == "rec709-camera");
  CHECK(grade.grade->color.outputColorSpace == "display-p3" && grade.grade->color.outputGamma == "linear");
  CHECK(app.themeIndex == 1 && !app.showRight);
  document_detail::setRawWorkingEncoding(app, RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate, options);
  CHECK(setOutputEncoding(app, {RgbGamut::ACES_AP0, TransferFunction::SRGB}));
  CHECK(app.rawWorkingEncoding == storedRaw && app.defaultOutputEncoding == storedOutput);
  CHECK(document_detail::openFolder(app, files.path("b"), {}, options));
  CHECK(fs::equivalent(app.path, b) && app.inputEncoding == storedRaw && app.outputEncoding == storedOutput);
  CHECK(app.themeIndex == 1 && !app.showRight && app.rawWorkingEncoding == storedRaw && app.defaultOutputEncoding == storedOutput);
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(app.inputEncoding == (ColorEncoding{RgbGamut::DaVinciWideGamut, TransferFunction::DaVinciIntermediate}));
  CHECK(app.outputEncoding == (ColorEncoding{RgbGamut::ACES_AP0, TransferFunction::SRGB}));
  CHECK(app.rawWorkingEncoding == storedRaw && app.defaultOutputEncoding == storedOutput);
  // A separately authored saved grade must take precedence over global defaults.
  const auto saved = newPhoto(files.path("b"), "saved.nef");
  write(inputSidecarPath(saved), R"({"format":"rawnode","version":1,"grade":{"color":{"rawWorking":{"colorSpace":"rec709","gamma":"linear"},"output":{"colorSpace":"aces-ap0","gamma":"srgb"}},"graph":{"primarySource":"saved","documentOutput":{"node":"saved","port":"image"},"nodes":[{"id":"saved","kind":"source"}]}}})");
  CHECK(document_detail::openPhoto(app, saved, {}, options));
  CHECK(app.inputEncoding == (ColorEncoding{RgbGamut::Rec709, TransferFunction::Linear}));
  CHECK(app.outputEncoding == (ColorEncoding{RgbGamut::ACES_AP0, TransferFunction::SRGB}));

  PersistGrade preset = *grade.grade;
  preset.color.rawColorSpace = "aces-ap0"; preset.color.rawGamma = "linear";
  preset.color.outputColorSpace = "rec2020"; preset.color.outputGamma = "srgb";
  CHECK(document_detail::applyGrade(app, preset, options));
  CHECK(app.photoRawWorkingEncoding == (ColorEncoding{RgbGamut::ACES_AP0, TransferFunction::Linear}));
  CHECK(app.outputEncoding == (ColorEncoding{RgbGamut::Rec2020, TransferFunction::SRGB}));
  CHECK(capturePreferences(app).rawWorking == storedRaw && capturePreferences(app).output == storedOutput);
  app.renderer.start(); CHECK(idle(app));
  const auto originalPreferences = bytes(config.preferences);
  options.beforeSave = [] { return Fault::Replace; };
  app.jpegQuality = 81;
  CHECK(!document_detail::prepareShutdown(app, config, true, {}, options));
  CHECK(!app.quit && bytes(config.preferences) == originalPreferences && app.path == saved);
  // Cancellation leaves the actual renderer/editor available for another edit.
  int previousGeneration;
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    previousGeneration = app.processed.generation;
  }
  CHECK(addNativeExposureNode(app));
  CHECK(setNodeParameterValue(app, app.nodes.back().id, "exposure", 1.0));
  app.renderer.requestPreview();
  CHECK(idle(app));
  const int requestedGeneration = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.processed.generation > previousGeneration);
    CHECK(app.processed.generation == requestedGeneration);
    CHECK(app.processed.result.state == EvaluationState::Available);
    const auto *rendered = std::get_if<ImageValue>(&app.processed.result.value);
    // Synthetic RAW is exactly 0.25; +1 stop must publish a new 0.5 raster.
    CHECK(rendered && *rendered && (*rendered)->px[0] == 0.5f);
  }
  CHECK(bytes(config.preferences) == originalPreferences && app.path == saved);
  options.beforeSave = {};
  CHECK(document_detail::prepareShutdown(app, config, true, {}, options));
  Preferences p;
  CHECK(loadPreferences(config.preferences, p, warning) && p.jpegQuality == 81 && p.rawWorking == storedRaw && p.output == storedOutput);
  CHECK(readGraphSidecar(inputSidecarPath(saved), grade, error) == SidecarReadStatus::Loaded && grade.grade->graph.nodes.size() == 2);
  CHECK(bytes(files.path("a/workspace.ofxrawhost.json")) == legacy);
  CHECK(bytes(files.path("a/.ofxrawhost-layout.ini")) == "legacy layout A\n");
  CHECK(entries(files.path("cwd")) == std::set<std::string>{"ofxrawhost.ini"});
  CHECK(bytes(files.path("cwd/ofxrawhost.ini")) == "legacy CWD layout\n");
  CHECK(!fs::exists(files.path("b/workspace.ofxrawhost.json")) && !fs::exists(files.path("b/.ofxrawhost-layout.ini")));
  CHECK(!fs::exists(files.path("a/preferences.json")) && !fs::exists(files.path("b/layout.ini")));
  app.renderer.shutdown();
  // Preferences save without any photo/folder having been opened.
  App empty; CHECK(initialisePreferences(empty, config)); empty.themeIndex = 0;
  CHECK(prepareShutdown(empty, config, true));
  CHECK(loadPreferences(config.preferences, p, warning) && p.themeIndex == 0);
  return true;
}

struct ImGuiContextScope {
  ImGuiContextScope() { ImGui::CreateContext(); }
  ~ImGuiContextScope() { ImGui::DestroyContext(); }
};
void beginFrame(ImVec2 displaySize = {1200, 800}) {
  auto &io = ImGui::GetIO(); io.DisplaySize = displaySize; io.DeltaTime = 1.0f / 60;
  unsigned char *pixels; int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  ImGui::NewFrame();
}

bool defaultPresetSettings() {
  Files files;
  const auto root = files.dir / "presets";
  const std::string nested = u8"b\u00e9ta/Look.RAWNODE.JSON";
  CHECK(fs::create_directories(root / "Alpha") && fs::create_directories(root / fs::u8path(nested).parent_path()));
  const auto preset = R"({"format":"rawnode","version":1,"grade":{"color":{"rawWorking":{"colorSpace":"aces-ap1","gamma":"linear"},"output":{"colorSpace":"display-p3","gamma":"srgb"}},"graph":{"primarySource":"preset-source","documentOutput":{"node":"preset-source","port":"image"},"nodes":[{"id":"preset-source","kind":"source"}]}}})";
  for (const auto &relative : {std::string("Alpha/Look.RAWNODE.JSON"), nested})
    CHECK(bool(std::ofstream(root / fs::u8path(relative)) << preset));
  CHECK(bool(std::ofstream(root / "z.rawnode.json") << "{"));
  CHECK(bool(std::ofstream(root / "ignore.json") << preset));

  ImGuiContextScope context;
  ImGuiBackend_ConfigureLayout("");
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
  App app;
  document_detail::ActionTestOptions options;
  options.decode = decodeRaw;
  CHECK(document_detail::openPhoto(app, newPhoto(files.dir, "photo.nef"), {}, options));
  const auto preview = app.preview;
  const auto sidecar = inputSidecarPath(app.path);
  const auto original = bytes(sidecar);
  app.settingsWindow.open = true;
  app.presetDirectory = root.u8string();
  app.defaultPreset = "missing.rawnode.json";
  const auto frame = [&](ImGuiID activate = 0) {
    if (activate) ImGui::ActivateItemByID(activate);
    beginFrame(); DrawUiFrame(app); ImGui::EndFrame();
  };
  frame(); frame();
  auto *settings = ImGui::FindWindowByName("Settings");
  CHECK(settings);
  const auto combo = settings->GetID("Default preset");
  frame(combo); frame();
  auto *popup = ImGui::FindWindowByName("##Combo_00");
  CHECK(popup && popup->Active);
  const std::vector<std::string> expected{"Alpha/Look.RAWNODE.JSON", nested, "z.rawnode.json"};
  CHECK(app.settingsWindow.defaultPresetChoices == expected);
  CHECK(app.settingsWindow.defaultPresetFeedback.find("missing.rawnode.json") != std::string::npos);
  CHECK(bool(std::ofstream(root / "new.rawnode.json") << preset));
  frame();
  CHECK(app.settingsWindow.defaultPresetChoices == expected);
  const int nestedIndex = 1;
  const auto nestedId = ImHashStr(nested.c_str(), 0, popup->GetID(nestedIndex));
  frame(nestedId); frame();
  CHECK(app.defaultPreset == nested && app.settingsWindow.defaultPresetFeedback.empty());
  CHECK(app.preview == preview && app.history.size() == 1 && app.photoRawWorkingEncoding == fallbackRaw);
  CHECK(app.outputEncoding == fallbackOutput && bytes(sidecar) == original);

  CHECK(fs::remove(root / fs::u8path(nested)));
  frame(combo); frame();
  CHECK(app.defaultPreset == nested && !app.settingsWindow.defaultPresetFeedback.empty());
  CHECK(app.settingsWindow.defaultPresetChoices ==
        (std::vector<std::string>{"Alpha/Look.RAWNODE.JSON", "new.rawnode.json", "z.rawnode.json"}));
  frame(ImHashStr("z.rawnode.json", 0, popup->GetID(2))); frame();
  CHECK(app.defaultPreset == "z.rawnode.json");
  CHECK(app.settingsWindow.defaultPresetFeedback.find("Could not read default preset") != std::string::npos);
  frame(combo); frame();
  frame(popup->GetID("None")); frame();
  CHECK(app.defaultPreset.empty() && app.settingsWindow.defaultPresetFeedback.empty());

  app.defaultPreset = "Alpha/Look.RAWNODE.JSON";
  frame(ImHashStr("Clear", 0, settings->GetID("Root preset directory")));
  CHECK(app.presetDirectory.empty() && app.defaultPreset.empty());
  CHECK(app.preview == preview && app.history.size() == 1 && bytes(sidecar) == original);
  return true;
}

bool keyboardShortcutSettings() {
  ImGuiContextScope context;
  ImGuiBackend_ConfigureLayout("");
  auto &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigInputTrickleEventQueue = false;
  App app;
  app.settingsWindow.open = true;
  app.settingsWindow.shortcutCommand = ui::ApplicationCommand::Filmstrip;
  const auto frame = [&](ImGuiID activate = 0) {
    if (activate) ImGui::ActivateItemByID(activate);
    beginFrame(); DrawUiFrame(app); ImGui::EndFrame();
  };
  const auto key = [&](ImGuiKey key, bool primary = false, ImGuiID activate = 0) {
    const auto modifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    if (primary) io.AddKeyEvent(modifier, true);
    io.AddKeyEvent(key, true);
    frame(activate);
    io.AddKeyEvent(key, false);
    if (primary) io.AddKeyEvent(modifier, false);
    frame();
  };

  frame();
  auto *settings = ImGui::FindWindowByName("Settings");
  CHECK(settings);
  settings->StateStorage.SetInt(settings->GetID("Keyboard Shortcuts"), 1);
  frame();
  frame(settings->GetID("Action"));
  frame();
  CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
  key(ImGuiKey_RightBracket, true);
  CHECK(app.showRight && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
  auto *combo = ImGui::FindWindowByName("##Combo_00");
  CHECK(combo && combo->Active);
  key(ImGuiKey_RightBracket, true, combo->GetID("Filmstrip"));
  CHECK(app.showRight && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));

  const auto change = ImHashStr("Change...", 0, settings->GetID(ImGuiMod_Ctrl | ImGuiKey_Backslash));
  frame(change);
  frame();
  CHECK(ImGui::GetTopMostPopupModal());
  key(ImGuiKey_RightBracket, true);
  CHECK(app.showRight && app.showFilmstrip && !app.keyboardBindings);
  CHECK(app.settingsWindow.shortcutFeedback == "Already assigned to Inspector.");
  key(ImGuiKey_LeftArrow);
  CHECK(!ImGui::GetTopMostPopupModal() && app.showFilmstrip && app.keyboardBindings);
  key(ImGuiKey_Backslash, true);
  CHECK(app.showFilmstrip);  // The replaced default has no second dispatch path.
  key(ImGuiKey_LeftArrow);
  CHECK(!app.showFilmstrip);
  key(ImGuiKey_LeftArrow);
  CHECK(app.showFilmstrip);

  frame(ImHashStr("Clear", 0, settings->GetID(ImGuiKey_LeftArrow)));
  frame();
  const auto &remaining = ui::effectiveKeyboardBindings(app);
  CHECK(std::none_of(remaining.begin(), remaining.end(), [](const auto &binding) {
    return binding.trigger == ImGuiKey_LeftArrow;
  }));
  key(ImGuiKey_LeftArrow);
  CHECK(app.showFilmstrip);
  frame(settings->GetID("Restore action defaults"));
  frame();
  key(ImGuiKey_Backslash, true);
  CHECK(!app.showFilmstrip);

  // The second alias conflicts. Restoring Filmstrip must retain both the
  // custom assignment and the other action's shortcut, without adding the first alias.
  using Command = ui::ApplicationCommand;
  app.keyboardBindings = ui::KeyboardBindings{
      {ImGuiKey_Home, {Command::Filmstrip}},
      {ImGuiMod_Super | ImGuiKey_Backslash, {Command::Inspector}},
  };
  frame(settings->GetID("Restore action defaults"));
  frame();
  CHECK(app.settingsWindow.shortcutFeedback.find("Already assigned to Inspector") != std::string::npos);
  const auto &bindings = ui::effectiveKeyboardBindings(app);
  CHECK(bindings.size() == 2 && bindings[0].trigger == ImGuiKey_Home && bindings[0].invokes(Command::Filmstrip));
  CHECK(bindings[1].trigger == (ImGuiMod_Super | ImGuiKey_Backslash) && bindings[1].invokes(Command::Inspector));

  frame(settings->GetID("Add shortcut..."));
  frame();
  CHECK(ImGui::GetTopMostPopupModal());
  key(ImGuiKey_Escape);
  CHECK(!ImGui::GetTopMostPopupModal() && app.keyboardBindings->size() == 2);

  const auto primary = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
  io.AddKeyEvent(primary, true);
  io.AddKeyEvent(ImGuiKey_Space, true);
  // Open and complete capture in one frame, leaving no popup at either dispatch check.
  frame(settings->GetID("Add shortcut..."));
  CHECK(!ImGui::GetTopMostPopupModal() && !app.showFilmstrip && app.keyboardBindings->size() == 3);
  io.AddKeyEvent(primary, false);
  frame();
  // Preview polls the level of Space while starting a pan.
  CHECK(!ImGui::IsKeyDown(ImGuiKey_Space));
  io.AddKeyEvent(ImGuiKey_Space, false);
  frame();
  key(ImGuiKey_Space, true);
  CHECK(app.showFilmstrip);
  frame(settings->GetID("Restore all defaults"));
  frame();
  CHECK(!app.keyboardBindings);
  // Create and reorder a sequence through the real Settings modal. Restoring
  // Filmstrip alone must keep a binding that happens to begin with Filmstrip.
  frame(settings->GetID("Add shortcut..."));
  frame();
  auto *capture = ImGui::GetTopMostPopupModal();
  CHECK(capture);
  frame(capture->GetID("Build sequence..."));
  frame();
  auto *editor = ImGui::GetTopMostPopupModal();
  CHECK(editor);
  ImGuiWindow *rows = nullptr;
  for (auto *child : editor->DC.ChildWindows) if (child->ChildId == editor->GetID("Commands")) rows = child;
  CHECK(rows);
  frame(rows->GetID("Add command"));
  frame();
  CHECK(app.settingsWindow.draftCommands.size() == 2);
  frame(ImHashStr("Up", 0, rows->GetID(1)));
  frame();
  CHECK(app.settingsWindow.draftCommands.front().kind == Command::SelectNext);
  frame(ImHashStr("Down", 0, rows->GetID(0)));
  frame();
  frame(editor->GetID("Assign shortcut..."));
  frame();
  key(ImGuiKey_F9);
  CHECK(app.keyboardBindings && app.keyboardBindings->back().trigger == ImGuiKey_F9);
  CHECK(!app.keyboardBindings->back().invokes(Command::Filmstrip));
  frame(settings->GetID("Restore action defaults"));
  frame();
  const auto &restored = *app.keyboardBindings;
  const auto sequence = std::find_if(restored.begin(), restored.end(), [](const auto &b) { return b.trigger == ImGuiKey_F9; });
  CHECK(sequence != restored.end() && sequence->commands.size() == 2);
  CHECK(sequence->commands[0].kind == Command::Filmstrip && sequence->commands[1].kind == Command::SelectNext);
  // Configure the same typed processor operand used by manual preferences
  // through a live-grade picker, then edit the saved binding in place.
  CHECK(addNativeExposureNode(app));
  app.settingsWindow.shortcutCommand = Command::SelectProcessor;
  frame(settings->GetID("Add shortcut..."));
  frame();
  frame(ImHashStr("Processor in grade", 0, rows->GetID(0)));
  frame();
  combo = ImGui::FindWindowByName("##Combo_00");
  CHECK(combo && combo->Active);
  const auto pickerNode = combo->GetID(app.selectedNodeId()->value.c_str());
  frame(ImHashStr("Exposure", 0, pickerNode));
  frame();
  frame(editor->GetID("Assign shortcut..."));
  frame();
  key(ImGuiKey_F10);
  const auto &picked = app.keyboardBindings->back();
  CHECK(picked.trigger == ImGuiKey_F10 && picked.invokes(Command::SelectProcessor));
  const auto &identity = std::get<ui::ProcessorIdentity>(picked.commands[0].operand);
  CHECK(identity.backend == "native" && identity.identifier == "rawnode.native.exposure");
  frame(ImHashStr("Edit commands...", 0, settings->GetID(ImGuiKey_F10)));
  frame();
  frame(rows->GetID("Add command"));
  frame();
  frame(ImHashStr("Remove", 0, rows->GetID(0)));
  frame();
  frame(editor->GetID("Save commands"));
  frame();
  CHECK(app.keyboardBindings->back().trigger == ImGuiKey_F10 && app.keyboardBindings->back().invokes(Command::SelectNext));
  return true;
}

bool warningGeometry() {
  const char *name = "Application preferences warning";
  for (const bool staleLayout : {false, true}) {
    ImGuiContextScope context;
    ImGuiBackend_ConfigureLayout("");
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (staleLayout)
      ImGui::LoadIniSettingsFromMemory("[Window][Application preferences warning]\nPos=60,60\nSize=36,500\nCollapsed=0\n\n");
    App app;
    app.preferencesWarning = "/a/long/configuration/path/" + std::string(240, 'x') +
        "/preferences.json: unsupported preferences version.\n"
        "Using in-memory defaults. The original file will remain untouched.";
    for (const auto display : {ImVec2{1200, 800}, ImVec2{420, 800}}) {
      // Auto-fit and content geometry settle over frames, as in the real UI.
      for (int frame = 0; frame < 3; ++frame) {
        beginFrame(display); DrawUiFrame(app); ImGui::EndFrame();
      }
      const auto *window = ImGui::FindWindowByName(name);
      CHECK(window && (window->Flags & ImGuiWindowFlags_NoSavedSettings));
      const float expectedWidth = display.x == 1200 ? 560.0f : 388.0f;
      CHECK(window->Size.x >= expectedWidth - 1 && window->Size.x <= expectedWidth + 1);
      CHECK(window->ContentSize.y > ImGui::GetTextLineHeight() * 2);
      CHECK(window->ContentSize.x <= window->InnerRect.GetWidth() + 1);
      CHECK(!app.preferencesWarning.empty());
    }
    if (!staleLayout) {
      const std::string saved = ImGui::SaveIniSettingsToMemory();
      CHECK(saved.find("[Window][Application preferences warning]") == std::string::npos);
    }
    // Restore the full viewport before clicking the close button; changing the
    // viewport above deliberately leaves ordinary window positioning to ImGui.
    for (int frame = 0; frame < 3; ++frame) {
      beginFrame(); DrawUiFrame(app); ImGui::EndFrame();
    }
    // Exercise the unchanged close-button path with real ImGui input events.
    const auto *window = ImGui::FindWindowByName(name);
    const ImVec2 close(window->Pos.x + window->Size.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() * 0.5f,
                       window->Pos.y + ImGui::GetStyle().FramePadding.y + ImGui::GetFontSize() * 0.5f);
    ImGui::SetWindowFocus(name);
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(close.x, close.y);
    beginFrame(); DrawUiFrame(app); ImGui::EndFrame();
    io.AddMouseButtonEvent(0, true);
    beginFrame(); DrawUiFrame(app); ImGui::EndFrame();
    io.AddMouseButtonEvent(0, false);
    beginFrame(); DrawUiFrame(app); ImGui::EndFrame();
    CHECK(app.preferencesWarning.empty());
  }
  return true;
}

bool globalLayout() {
  Files files; fs::create_directory(files.path("cwd")); fs::create_directory(files.path("folder"));
  CurrentDirectory cwd(files.path("cwd"));
  const std::string layout = files.path("layout.ini");
  write(layout, "[Window][Preferences test]\nPos=31,47\nSize=300,200\nCollapsed=0\n\n");
  {
    ImGuiContextScope context;
    ImGuiBackend_ConfigureLayout(layout);
    CHECK(std::string(ImGui::GetIO().IniFilename) == layout);
    beginFrame(); ImGui::Begin("Preferences test");
    CHECK(ImGui::GetWindowPos().x == 31 && ImGui::GetWindowPos().y == 47);
    ImGui::SetWindowPos({79, 83}); ImGui::End(); ImGui::EndFrame();
    const auto *stablePath = ImGui::GetIO().IniFilename;
    App app; CHECK(openFolder(app, files.path("folder")));
    CHECK(ImGui::GetIO().IniFilename == stablePath && std::string(stablePath) == layout);
    ImGui::SaveIniSettingsToDisk(stablePath);
  }
  {
    ImGuiContextScope context; ImGuiBackend_ConfigureLayout(layout);
    beginFrame(); ImGui::Begin("Preferences test");
    CHECK(ImGui::GetWindowPos().x == 79 && ImGui::GetWindowPos().y == 83);
    ImGui::End(); ImGui::EndFrame();
  }
  {
    ImGuiContextScope context; ImGuiBackend_ConfigureLayout("");
    CHECK(ImGui::GetIO().IniFilename == nullptr);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    App app;
    beginFrame(); DockLayout::BeginMainDockSpace(app); ImGui::EndFrame();
    CHECK(ImGui::DockBuilderGetNode(ImHashStr("OfxRawHostDock", 0, ImHashStr("##DockHost"))) != nullptr);
  }
  CHECK(entries(files.path("cwd")).empty() && entries(files.path("folder")).empty());
  return true;
}
}  // namespace

bool testPreferences() {
  for (const auto &test : {
       std::make_pair("codec, optional fields and pair fallbacks", codecAndOptionalFields),
       std::make_pair("typed keyboard commands and malformed sequence isolation", typedBindingRoundTrip),
       std::make_pair("keyboard defaults match the base revision", defaultKeyboardBindings),
       std::make_pair("keyboard binding preference compatibility, recovery and restart", keyboardBindingsLifecycle),
       std::make_pair("keyboard shortcut Settings capture, conflicts, clearing and restore", keyboardShortcutSettings),
       std::make_pair("List/Graph preference compatibility and validation", nodesViewPreference),
       std::make_pair("directory preferences persist and clear across startup and shutdown", directoryPreferencesLifecycle),
       std::make_pair("malformed and unavailable directory preferences", invalidDirectoryPreferences),
       std::make_pair("optional UTF-8 default preset persists and validates independently", defaultPresetPreference),
       std::make_pair("default preset Settings discovers on demand and preserves the current grade", defaultPresetSettings),
       std::make_pair("config paths and checked atomic failures", atomicFailuresAndPaths),
       std::make_pair("unsupported/malformed preferences survive shutdown", protectedPreferencesShutdown),
       std::make_pair("startup colours, folder retirement and shutdown cancellation", startupPhotoFolderAndShutdown),
       std::make_pair("global native ImGui layout", globalLayout),
       std::make_pair("warning geometry, stale ini and dismissal", warningGeometry)}) {
    if (!test.second()) { std::fprintf(stderr, "FAILED preferences: %s\n", test.first); return false; }
    std::printf("ok  Step 7 preferences: %s\n", test.first);
  }
  return true;
}
