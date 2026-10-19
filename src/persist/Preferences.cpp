#include "persist/Preferences.h"
#include "persist/PreferencesPriv.h"
#include "persist/Json.h"
#include "AppState.h"
#include "ui/Themes.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"

#include <cstdlib>
#include <filesystem>
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
std::string pairJson(ColorEncoding encoding) {
  return "{\"colorSpace\":" + jsonStringValue(rgbGamutId(encoding.gamut)) +
         ",\"gamma\":" + jsonStringValue(transferFunctionId(encoding.gamma)) + "}";
}
}  // namespace

ConfigPaths preferences_detail::configPaths(const std::string &home, const std::string &configHome,
                                            std::string &warning) {
  warning.clear();
  fs::path directory;
#if defined(__APPLE__)
  if (fs::path(home).is_absolute()) directory = fs::path(home) / "Library/Application Support/RawNode";
#elif defined(_WIN32)
  if (fs::path(configHome).is_absolute()) directory = fs::path(configHome) / "RawNode";
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
    warning = directory.string() + ": " + ec.message() + "; using in-memory settings.";
    return {};
  }
  ConfigPaths paths{(directory / "preferences.json").string(), (directory / "layout.ini").string()};
  // ImGui has no error channel for ini writes. Reject an unusable destination
  // now and keep in-memory layout; never choose another directory.
  const auto status = fs::symlink_status(paths.layout, ec);
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
  const auto env = [](const char *name) { const auto *value = std::getenv(name); return value ? value : ""; };
#ifdef _WIN32
  return preferences_detail::configPaths(env("USERPROFILE"), env("APPDATA"), warning);
#else
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
  boolean(root, "showLeft", out.showLeft, warning);
  boolean(root, "showRight", out.showRight, warning);
  boolean(root, "showNodes", out.showNodes, warning);
  if (const auto *value = optional(root, "nodesView")) {
    if (value->type == Type::String && value->string == "graph") out.nodesView = NodesView::Graph;
    else if (value->type != Type::String || value->string != "list")
      warn(warning, "nodesView: expected list or graph; using List View");
  }
  boolean(root, "showFilmstrip", out.showFilmstrip, warning);
  size(root, "leftW", out.leftW, 1, 10000, warning);
  size(root, "rightW", out.rightW, 1, 10000, warning);
  size(root, "filmstripH", out.filmstripH, 48, 240, warning);
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
      << ",\n  \"showLeft\":" << (p.showLeft ? "true" : "false")
      << ",\n  \"showRight\":" << (p.showRight ? "true" : "false")
      << ",\n  \"showNodes\":" << (p.showNodes ? "true" : "false")
      << ",\n  \"nodesView\":\"" << (p.nodesView == NodesView::Graph ? "graph" : "list") << "\""
      << ",\n  \"showFilmstrip\":" << (p.showFilmstrip ? "true" : "false")
      << ",\n  \"leftW\":" << p.leftW << ",\n  \"rightW\":" << p.rightW
      << ",\n  \"filmstripH\":" << p.filmstripH << "\n}\n";
  return persist_detail::replaceFileAtomically(path, out.str(), error, fault);
}

bool savePreferences(const std::string &path, const Preferences &preferences, std::string &error) {
  return preferences_detail::save(path, preferences, error, persist_detail::FileFault::None);
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
  p.showLeft = app.showLeft; p.showRight = app.showRight; p.showFilmstrip = app.showFilmstrip;
  p.leftW = app.leftW; p.rightW = app.rightW; p.filmstripH = app.filmstripH;
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
  app.showLeft = p.showLeft; app.showRight = p.showRight; app.showFilmstrip = p.showFilmstrip;
  app.leftW = p.leftW; app.rightW = p.rightW; app.filmstripH = p.filmstripH;
}

bool initialisePreferences(App &app, const ConfigPaths &paths) {
  Preferences preferences;
  std::string warning;
  const bool loaded = loadPreferences(paths.preferences, preferences, warning);
  applyPreferences(app, preferences);
  if (!warning.empty()) warn(app.preferencesWarning, warning);
  return loaded;
}
