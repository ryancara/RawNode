#pragma once

#include "color/ColorEncoding.h"
#include <string>

struct App;

enum class NodesView { List, Graph };

// Application defaults only. Photo colours and ImGui-owned geometry are separate.
// New settings can be optional fields with defaults; no migration is needed.
struct Preferences {
  ColorEncoding rawWorking{RgbGamut::Rec2020, TransferFunction::Linear};
  ColorEncoding output{RgbGamut::Rec709, TransferFunction::SRGB};
  int exportFormat = 1;
  int jpegQuality = 92;
  int previewRes = 1;
  int themeIndex = 2;
  bool showRight = true;
  bool showNodes = true;
  NodesView nodesView = NodesView::List;
  bool showFilmstrip = true;
  bool showPresetPanel = false;
  float rightW = 420.0f;
  float filmstripH = 96.0f;
  // Absolute UTF-8 paths. Empty means no directory is configured.
  std::string presetDirectory;
  std::string ctlScriptsDirectory;
  // UTF-8 path relative to presetDirectory. Empty means no default preset.
  std::string defaultPreset;
};

struct ConfigPaths {
  std::string preferences;
  std::string layout;
};

// Resolve and prepare once at startup. Failure returns empty paths, never CWD.
ConfigPaths applicationConfigPaths(std::string &warning);
// Missing file succeeds with built-in defaults. Invalid individual optional
// fields warn and default independently. A file-level error returns false.
bool loadPreferences(const std::string &path, Preferences &out, std::string &warning);
bool savePreferences(const std::string &path, const Preferences &preferences, std::string &error);
// Check a configured or picked directory on demand. Empty paths are valid.
std::string directoryPreferenceError(const std::string &path);
bool validDefaultPresetPath(const std::string &path);
Preferences capturePreferences(const App &app);
void applyPreferences(App &app, const Preferences &preferences);
// Production startup seam: load and apply before ImGui or the first photograph.
bool initialisePreferences(App &app, const ConfigPaths &paths);
