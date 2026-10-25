#include "persist/DocumentActions.h"
#include "persist/DocumentActionsPriv.h"
#include "persist/Preferences.h"

#include "ui/Filmstrip.h"
#include "imgio/ImageIO.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "NodeGraph.h"
#include "DocumentMutation.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

PersistGradeColor captureGradeColor(const App &app) {
  PersistGradeColor color;
  std::lock_guard<std::mutex> lock(app.colorMutex);
  color.rawColorSpace = rgbGamutId(app.photoRawWorkingEncoding.gamut);
  color.rawGamma = transferFunctionId(app.photoRawWorkingEncoding.gamma);
  color.outputColorSpace = rgbGamutId(app.outputEncoding.gamut);
  color.outputGamma = transferFunctionId(app.outputEncoding.gamma);
  return color;
}

void document_detail::setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma,
                                           const ActionTestOptions &options) {
  if (!requireEditableGraph(app) || app.path.empty()) return;
  const ColorEncoding requested{gamut, gamma};
  {
    document_detail::DocumentMutation mutation(app);
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      if (!app.inputIsRaw || app.photoRawWorkingEncoding == requested) return;
    }
    Image decoded;
    ColorEncoding detected;
    bool raw = false;
    if (!options.decode(app.path, decoded, detected, raw, requested) || !raw) {
      app.setStatus("Could not reload RAW in " + colorEncodingName(requested));
      return;
    }
    auto preview = document_detail::makePreviewSource(decoded, detected, app.previewRes);
    app.full.swap(decoded);
    app.preview.swap(preview);
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      app.inputEncoding = detected;
      app.photoRawWorkingEncoding = requested;
    }
    mutation.changed();
    app.history.recordCompletedEdit(app, &mutation);
  }
  app.setStatus("RAW working encoding: " + colorEncodingName(requested));
}

void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma) {
  document_detail::setRawWorkingEncoding(app, gamut, gamma, {});
}

bool setOutputEncoding(App &app, ColorEncoding encoding) {
  if (!requireEditableGraph(app) || app.path.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    if (app.outputEncoding == encoding) return true;
    app.outputEncoding = encoding;
  }
  app.history.recordMetadataChange(app);
  app.renderer.requestDisplayRefresh();
  return true;
}

static bool captureGradeState(App &app, PersistGrade &grade, std::map<std::string, bool> *workingProcessors) {
  if (!requireEditableGraph(app)) return false;
  try {
    PersistGrade captured;
    captured.graph = captureGraph({app.nodes, app.primarySource, app.documentOutput});
    captured.color = captureGradeColor(app);
    captured.positions = app.nodePositions;
    std::map<std::string, bool> availability;
    if (workingProcessors) for (const auto &node : app.nodes)
      if (node.operation == NodeOperation::ImageProcessor)
        availability.emplace(node.id.value, bool(node.processor));
    std::string json, error;
    PersistFile file; file.grade = captured;
    if (!encodeGraphFile(file, json, error) || !decodeGraphFile(json, file, error)) throw std::runtime_error(error);
    grade = std::move(*file.grade);
    if (workingProcessors) workingProcessors->swap(availability);
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not capture grade: " + std::string(failure.what()));
    return false;
  }
}

bool captureGrade(App &app, PersistGrade &grade) {
  if (!requireEditableGraph(app)) return false;
  document_detail::DocumentMutation mutation(app);
  return captureGradeState(app, grade, nullptr);
}

bool document_detail::captureGradeSnapshot(App &app, GradeSnapshot &snapshot, const DocumentMutation *mutation) {
  DocumentMutation::assertControlThread(app);
  if (mutation) {
    assert(mutation->owns(app));
    return captureGradeState(app, snapshot.grade, &snapshot.workingProcessors);
  }
  if (!app.renderer.canCaptureSnapshot()) {
    app.setStatus("Cannot capture history during export, document mutation or protection");
    return false;
  }
  DocumentMutation owned(app);
  return captureGradeState(app, snapshot.grade, &snapshot.workingProcessors);
}

bool captureNodeFragment(App &app, const std::vector<NodeId> &ids, PersistFragment &fragment,
                         std::vector<std::string> &warnings) {
  document_detail::DocumentMutation::assertControlThread(app);
  if (!requireEditableGraph(app)) return false;
  if (ids.empty()) { app.setStatus("No nodes to capture"); return false; }
  // Topology and identity belong to the control thread; processor state is
  // synchronised/read only after DocumentMutation drains preview and export.
  for (auto it = ids.begin(); it != ids.end(); ++it) {
    const Node *node = findNode(app, *it);
    if (!node || node->operation == NodeOperation::Source || std::find(ids.begin(), it, *it) != it) {
      app.setStatus("Cannot copy Source, stale or duplicate nodes");
      return false;
    }
  }
  document_detail::DocumentMutation mutation(app);
  try {
    std::vector<std::string> capturedWarnings;
    auto captured = captureFragment(app, ids, capturedWarnings);
    fragment = std::move(captured);
    warnings = std::move(capturedWarnings);
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not capture nodes: " + std::string(failure.what()));
    return false;
  }
}

bool document_detail::savePreset(App &app, const std::string &path, const OverwritePrompt &confirm,
                                 const ActionTestOptions &options) {
  if (!requireEditableGraph(app)) return false;
  PersistGrade grade;
  if (!captureGrade(app, grade)) return false;
  std::string error, json;
  const auto status = persist_detail::readGraphFileText(path, json, error, options.readFault);
  if (status == SidecarReadStatus::Error) { app.setStatus(error); return false; }
  if (status == SidecarReadStatus::Loaded && (!confirm || !confirm(path))) return false;
  const auto fault = options.beforeSave ? options.beforeSave() : persist_detail::FileFault::None;
  if (!persist_detail::updateGraphSidecar(path, &grade, PhotoMark::Unmarked, error, fault, options.testOperations)) {
    app.setStatus("Could not save preset: " + error); return false;
  }
  app.setStatus("Saved preset: " + fs::path(path).filename().string());
  return true;
}
bool savePreset(App &app, const std::string &path, const OverwritePrompt &confirm) {
  return document_detail::savePreset(app, path, confirm, {});
}

static bool sameNodeIdentity(const Node &a, const Node &b) {
  return a.id == b.id && a.operation == b.operation &&
         a.storedBackend == b.storedBackend && a.storedIdentifier == b.storedIdentifier;
}

static bool applyGradeState(App &app, const PersistGrade &incoming,
                            const document_detail::ActionTestOptions &options,
                            const std::map<std::string, bool> *workingProcessors) {
  if (!requireEditableGraph(app)) return false;
  document_detail::DocumentMutation::assertControlThread(app);
  try {
    // Validate through the shared grade codec before constructing processors;
    // normalization applies exactly the same position/default rules as opening.
    PersistFile file; file.grade = incoming;
    std::string json, error;
    if (!persist_detail::encodeGraphFile(file, json, error, nullptr, options.testOperations) ||
        !persist_detail::decodeGraphFile(json, file, error, nullptr, options.testOperations)) throw std::runtime_error(error);
    auto grade = std::move(*file.grade);
    ColorEncoding raw, output;
    if (!colorEncodingFromIds(grade.color.rawColorSpace, grade.color.rawGamma, raw) ||
        !colorEncodingFromIds(grade.color.outputColorSpace, grade.color.outputGamma, output))
      throw std::runtime_error("Invalid full-grade colour settings");
    // Malformed/deep input is refused without interrupting preview/export.
    // Backend construction still requires the existing execution exclusion.
    document_detail::DocumentMutation mutation(app);
    auto restored = document_detail::restoreGraph(grade.graph, workingProcessors);
    // View state belongs to the logical node, not just its potentially reused ID.
    // Stage expansion copies and reset decisions before any graph publication.
    bool clearSelection = false;
    bool changedNodeIdentity = false;
    if (workingProcessors) {
      const auto selection = app.selectedNodeId();
      clearSelection = selection.has_value();
      for (auto &node : restored.nodes) {
        const auto *live = findNode(app, node.id);
        if (!live) continue;
        if (!sameNodeIdentity(*live, node)) {
          changedNodeIdentity = true;
          continue;
        }
        node.groupOpen = live->groupOpen;
        if (selection && *selection == node.id) clearSelection = false;
      }
    }
    bool reloadRaw;
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      reloadRaw = app.inputIsRaw && app.inputEncoding != raw;
    }
    Image decoded;
    ColorEncoding detected;
    std::shared_ptr<const Image> preview;
    if (reloadRaw) {
      bool decodedRaw = false;
      if (!options.decode(app.path, decoded, detected, decodedRaw, raw) || !decodedRaw) {
        app.setStatus("Could not reload RAW in " + colorEncodingName(raw));
        return false;
      }
      preview = options.preparePreview(decoded, detected, app.previewRes);
    }
    // All fallible work precedes publication. Retired nodes/pixels are destroyed
    // while this DocumentMutation still excludes preview and full export.
    app.nodes.swap(restored.nodes);
    std::swap(app.primarySource, restored.primarySource);
    std::swap(app.documentOutput, restored.output);
    app.nodePositions.swap(grade.positions);
    if (reloadRaw) { app.full.swap(decoded); app.preview.swap(preview); }
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      app.outputEncoding = output;
      app.photoRawWorkingEncoding = raw;
      if (reloadRaw) app.inputEncoding = detected;
    }
    if (!workingProcessors) {
      selectNode(app, std::nullopt);
      app.paramFilter[0] = '\0';
      app.renderer.resetInspection();
    } else {
      if (clearSelection) selectNode(app, std::nullopt);
      if (changedNodeIdentity) app.renderer.resetInspection();
    }
    // DocumentMutation already clears an invalid inspected endpoint and keeps
    // a valid one when scheduling the completed graph.
    mutation.changed();
    if (!workingProcessors) app.history.recordCompletedEdit(app, &mutation);
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not apply full grade: " + std::string(failure.what()));
    return false;
  }
}

bool document_detail::applyGrade(App &app, const PersistGrade &grade, const ActionTestOptions &options) {
  return applyGradeState(app, grade, options, nullptr);
}

bool document_detail::restoreGradeSnapshot(App &app, const GradeSnapshot &snapshot, const ActionTestOptions &options) {
  // History replay is unavailable during export; ordinary replacement retains
  // its existing drain-and-replace behaviour. Both use the same staged path.
  if (!app.renderer.canEditParameters()) return false;
  return applyGradeState(app, snapshot.grade, options, &snapshot.workingProcessors);
}

bool applyGrade(App &app, const PersistGrade &grade) {
  return document_detail::applyGrade(app, grade, {});
}

namespace document_detail {
static bool savePhotoWithinMutation(App &app, std::string &error, const ActionTestOptions &options) {
  try {
    PersistGrade grade;
    grade.graph = captureGraph({app.nodes, app.primarySource, app.documentOutput});
    grade.color = captureGradeColor(app);
    grade.positions = app.nodePositions;
    const auto fault = options.beforeSave ? options.beforeSave() : persist_detail::FileFault::None;
    if (persist_detail::updateGraphSidecar(inputSidecarPath(app.path), &grade, PhotoMark::Unmarked,
                                         error, fault, options.testOperations)) {
      app.persistenceWarning.clear();
      return true;
    }
  } catch (const std::exception &failure) {
    error = inputSidecarPath(app.path) + ": " + failure.what();
  }
  app.persistenceWarning = "Could not save grade: " + error;
  app.setStatus(app.persistenceWarning);
  return false;
}

bool savePhoto(App &app, std::string &error, const ActionTestOptions &options) {
  error.clear();
  if (app.path.empty()) return true;
  if (app.isPhotoProtected()) {
    error = "The saved grade was not loaded; grade saving is disabled";
    app.setStatus(error);
    return false;
  }
  DocumentMutation mutation(app);
  return savePhotoWithinMutation(app, error, options);
}

bool leavePhoto(App &app, const SaveFailurePrompt &prompt, const ActionTestOptions &options) {
  if (app.path.empty() || app.isPhotoProtected()) return true;
  std::string error;
  while (!savePhoto(app, error, options)) {
    const auto decision = prompt ? prompt(error) : SaveFailureDecision::Cancel;
    switch (decision) {
      case SaveFailureDecision::Retry: break;
      case SaveFailureDecision::Discard: return true;
      case SaveFailureDecision::Cancel: return false;
    }
  }
  return true;
}
}  // namespace document_detail

bool saveCurrentInputSidecar(App &app, std::string *failure) {
  std::string error;
  const bool saved = document_detail::savePhoto(app, error, {});
  if (failure) *failure = error;
  if (saved && !app.path.empty()) app.setStatus("Saved grade");
  return saved;
}

bool preparePhotoDeparture(App &app, const SaveFailurePrompt &prompt) {
  return document_detail::leavePhoto(app, prompt, {});
}

bool document_detail::prepareShutdown(App &app, const ConfigPaths &config, bool preferencesLoaded,
                                      const SaveFailurePrompt &prompt, const ActionTestOptions &options) {
  if (!leavePhoto(app, prompt, options)) return false;
  if (preferencesLoaded && !config.preferences.empty()) {
    std::string error;
    if (!savePreferences(config.preferences, capturePreferences(app), error))
      app.preferencesWarning = "Could not save application preferences: " + error;
  }
  return true;
}

bool prepareShutdown(App &app, const ConfigPaths &config, bool preferencesLoaded, const SaveFailurePrompt &prompt) {
  return document_detail::prepareShutdown(app, config, preferencesLoaded, prompt, {});
}

namespace document_detail {
// Caller has resolved departure before reading the incoming file. This is also
// used by folder auto-open so explicit Discard does not trigger a second save.
static bool openPreparedPhoto(App &app, const std::string &path, const ActionTestOptions &options) {
  DocumentMutation mutation(app);
  try {
    std::string json, error;
    const auto status = persist_detail::readGraphFileText(inputSidecarPath(path), json, error, options.readFault);
    PersistFile file;
    bool protectedSource = status == SidecarReadStatus::Error;
    if (status == SidecarReadStatus::Loaded &&
        !persist_detail::decodeGraphFile(json, file, error, nullptr, options.testOperations)) {
      error = inputSidecarPath(path) + ": " + error;
      protectedSource = true;
    }
    const bool initialSave = !protectedSource && !file.grade;
    ColorEncoding raw, output;
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      raw = app.rawWorkingEncoding;
      output = app.defaultOutputEncoding;
    }
    RestoredGraph restored;
    PersistPositions positions;
    if (!protectedSource && file.grade) {
      try {
        restored = restoreGraph(file.grade->graph);
        if (!colorEncodingFromIds(file.grade->color.rawColorSpace, file.grade->color.rawGamma, raw) ||
            !colorEncodingFromIds(file.grade->color.outputColorSpace, file.grade->color.outputGamma, output))
          throw std::runtime_error("Invalid grade colour settings");
        positions = file.grade->positions;
      } catch (const std::exception &failure) {
        error = inputSidecarPath(path) + ": " + failure.what();
        protectedSource = true;
      }
    }
    if (protectedSource || !file.grade) {
      RestoredGraph sourceOnly;
      Node source(NodeOperation::Source);
      sourceOnly.primarySource = source.id = NodeId{"source"};
      sourceOnly.output = {source.id, ports::image};
      sourceOnly.nodes.push_back(std::move(source));
      restored = std::move(sourceOnly);
      positions.clear();
      if (protectedSource) {
        raw = {RgbGamut::Rec709, TransferFunction::Linear};
        output = {RgbGamut::Rec709, TransferFunction::SRGB};
      }
    }
    Image decoded;
    ColorEncoding detected;
    bool decodedRaw = false;
    if (!options.decode(path, decoded, detected, decodedRaw, raw)) {
      app.setStatus("Could not decode " + path);
      return false;
    }
    auto preview = makePreviewSource(decoded, detected, app.previewRes);
    std::string incomingPath = path;
    std::string blockedPath = protectedSource ? path : "";
    std::string warning = protectedSource ? "Saved grade was not loaded; this photo is read-only: " + error
        : app.path == path ? app.persistenceWarning : std::string();
    auto loaded = "Loaded " + fs::path(path).filename().string() + " (" + colorEncodingName(detected) + ")";
    int filmstripIndex = -1;
    for (size_t i = 0; i < app.filmstrip.size(); ++i) {
      std::error_code ec;
      if (fs::equivalent(app.filmstrip[i].path, path, ec)) { filmstripIndex = int(i); break; }
    }
    // Every fallible decode, reconstruction and preview allocation precedes
    // publication. Old processors/pixels remain owned inside this mutation.
    app.nodes.swap(restored.nodes);
    std::swap(app.primarySource, restored.primarySource);
    std::swap(app.documentOutput, restored.output);
    app.nodePositions.swap(positions);
    app.path.swap(incomingPath);
    app.full.swap(decoded);
    app.preview.swap(preview);
    app.sidecarWriteBlockedPath.swap(blockedPath);
    app.persistenceWarning.swap(warning);
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      app.inputEncoding = detected;
      app.inputIsRaw = decodedRaw;
      app.photoRawWorkingEncoding = raw;
      app.outputEncoding = output;
    }
    selectNode(app, std::nullopt);
    app.paramFilter[0] = '\0';
    app.renderer.resetInspection();
    app.previewZoom = 1.0f;
    app.previewPanX = app.previewPanY = 0.0f;
    app.filmstripIndex = filmstripIndex;
    {
      std::lock_guard<std::mutex> lock(app.statusMutex);
      app.status.swap(loaded);
    }
    mutation.changed();
    if (initialSave) { std::string failure; savePhotoWithinMutation(app, failure, options); }
    app.history.reset(app, &mutation);
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not open " + path + ": " + failure.what());
    return false;
  }
}

bool openPhoto(App &app, const std::string &path, const SaveFailurePrompt &prompt,
               const ActionTestOptions &options) {
  if (isHostMetadataPath(path)) {
    app.setStatus("Sidecar files are not images — open the image file instead.");
    return false;
  }
  if (!leavePhoto(app, prompt, options)) return false;
  const bool opened = openPreparedPhoto(app, path, options);
  return opened;
}

bool openFolder(App &app, const std::string &dir, const SaveFailurePrompt &prompt,
                const ActionTestOptions &options) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) { app.setStatus("Not a directory"); return false; }
  auto folder = fs::weakly_canonical(fs::path(dir), ec).string();
  if (ec) { app.setStatus("Could not open folder: " + ec.message()); return false; }
  if (!leavePhoto(app, prompt, options)) return false;
  app.folderDir.swap(folder);
  refreshFilmstrip(app);
  std::string toOpen;
  if (toOpen.empty() && !app.filmstrip.empty()) toOpen = app.filmstrip.front().path;
  if (!toOpen.empty()) openPreparedPhoto(app, toOpen, options);
  else app.setStatus("Folder: " + app.folderDir + " (no images)");
  return true;
}
}  // namespace document_detail

bool openFolder(App &app, const std::string &dir, const SaveFailurePrompt &prompt) {
  return document_detail::openFolder(app, dir, prompt, {});
}
bool openPath(App &app, const std::string &path, const SaveFailurePrompt &prompt) {
  return document_detail::openPhoto(app, path, prompt, {});
}
