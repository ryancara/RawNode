#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"
#include <functional>

// Control-thread document/workspace actions that connect live App state to
// durable records. ProjectPersist owns the formats; NodeGraph restores nodes.
PersistGui captureGui(const App &app);
// Per-image metadata excludes the workspace's RAW session defaults.
PersistGui captureSidecarGui(const App &app);
void applyGui(App &app, const PersistGui &g);
PersistGradeColor captureGradeColor(const App &app);
// Clipboard/preset grade replacement: prepare all nodes and colour/source
// changes before committing. Failure leaves the current document intact.
bool applyGrade(App &app, const PersistGrade &grade);
// Complete capture also supports editable Source-only grades. Owns execution
// exclusion while reading processor parameters, and excludes photo metadata.
bool captureGrade(App &app, PersistGrade &grade);
using OverwritePrompt = std::function<bool(const std::string &path)>;
bool savePreset(App &app, const std::string &path, const OverwritePrompt &confirm = {});
enum class SaveFailureDecision { Retry, Discard, Cancel };
using SaveFailurePrompt = std::function<SaveFailureDecision(const std::string &error)>;
bool saveCurrentInputSidecar(App &app, std::string *error = nullptr);
// Resolve before replacement or shutdown intent. An absent prompt cancels a
// failed departure; only an explicit Discard permits leaving without saving.
bool preparePhotoDeparture(App &app, const SaveFailurePrompt &prompt = {});
void persistWorkspace(App &app);
bool openWorkspace(App &app, const std::string &dir, const SaveFailurePrompt &prompt = {});
bool openPath(App &app, const std::string &path, const SaveFailurePrompt &prompt = {});
void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma);
bool setOutputEncoding(App &app, ColorEncoding encoding);
