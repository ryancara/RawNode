#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

PersistGui captureGui(const App &app);
void applyGui(App &app, const PersistGui &g);
PersistGradeColor captureGradeColor(const App &app);
bool applyGradeColor(App &app, const PersistGradeColor &color);
void saveCurrentInputSidecar(App &app);
void persistWorkspace(App &app);
void openWorkspace(App &app, const std::string &dir);
void openPath(App &app, const std::string &path, bool applySidecar = true);
void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma);
void doExport(App &app);
// Both paths capture inputs on the caller/control thread and use the same
// synchronous export body. Each owns full-resolution execution until cleanup.
bool runExportJob(App &app, const std::string &outPath);
bool startExport(App &app, const std::string &outPath);
