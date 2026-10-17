#pragma once

#include "color/ColorEncoding.h"
#include <string>

struct App;

// Application defaults only. Photo colours and ImGui-owned geometry are separate.
// New settings can be optional fields with defaults; no migration is needed.
struct Preferences {
  ColorEncoding rawWorking{RgbGamut::Rec2020, TransferFunction::Linear};
  ColorEncoding output{RgbGamut::Rec709, TransferFunction::SRGB};
  int exportFormat = 1;
  int jpegQuality = 92;
  int previewRes = 1;
  int themeIndex = 2;
  bool showLeft = true;
  bool showRight = true;
  bool showFilmstrip = true;
  float leftW = 280.0f;
  float rightW = 420.0f;
  float filmstripH = 96.0f;
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
Preferences capturePreferences(const App &app);
void applyPreferences(App &app, const Preferences &preferences);
// Production startup seam: load and apply before ImGui or the first photograph.
bool initialisePreferences(App &app, const ConfigPaths &paths);
