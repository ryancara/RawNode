#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"
#include <functional>

// Control-thread photo/folder actions. ProjectPersist owns durable records;
// NodeGraph restores nodes. Folder selection is transient.
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
bool prepareShutdown(App &app, const std::string &preferencesPath, const SaveFailurePrompt &prompt = {});
bool openFolder(App &app, const std::string &dir, const SaveFailurePrompt &prompt = {});
bool openPath(App &app, const std::string &path, const SaveFailurePrompt &prompt = {});
void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma);
bool setOutputEncoding(App &app, ColorEncoding encoding);
