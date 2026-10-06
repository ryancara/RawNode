#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

// Control-thread document/workspace actions that connect live App state to
// durable records. ProjectPersist owns the formats; NodeGraph restores nodes.
PersistGui captureGui(const App &app);
// Per-image metadata excludes the workspace's RAW session defaults.
PersistGui captureSidecarGui(const App &app);
void applyGui(App &app, const PersistGui &g);
PersistGradeColor captureGradeColor(const App &app);
bool applyGradeColor(App &app, const PersistGradeColor &color);
void saveCurrentInputSidecar(App &app);
void persistWorkspace(App &app);
void openWorkspace(App &app, const std::string &dir);
void openPath(App &app, const std::string &path, bool applySidecar = true);
void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma);
