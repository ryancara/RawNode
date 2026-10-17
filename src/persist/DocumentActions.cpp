#include "persist/DocumentActions.h"
#include "persist/DocumentActionsPriv.h"

#include "ui/Filmstrip.h"
#include "imgio/ImageIO.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "NodeGraph.h"
#include "DocumentMutation.h"
#include "ui/Themes.h"
#include "ui/ImGuiBackend.h"

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

static bool persistedColorEncoding(const std::string &gamutId,
                                   const std::string &transferId,
                                   ColorEncoding &encoding) {
  if (colorEncodingFromIds(gamutId, transferId, encoding)) return true;

  // Persistence-only migration for development builds that briefly wrote
  // display names instead of stable IDs. Runtime colour APIs remain IDs-only.
  RgbGamut gamut;
  if (gamutId == "Rec.709") gamut = RgbGamut::Rec709;
  else if (gamutId == "Rec.2020") gamut = RgbGamut::Rec2020;
  else if (gamutId == "Display P3") gamut = RgbGamut::DisplayP3;
  else if (gamutId == "ACES AP0" || gamutId == "ACES2065-1" || gamutId == "AP0") gamut = RgbGamut::ACES_AP0;
  else if (gamutId == "ACES AP1" || gamutId == "ACEScg" || gamutId == "AP1") gamut = RgbGamut::ACES_AP1;
  else if (gamutId == "DaVinci Wide Gamut") gamut = RgbGamut::DaVinciWideGamut;
  else return false;

  TransferFunction gamma;
  if (transferId == "Linear") gamma = TransferFunction::Linear;
  else if (transferId == "sRGB") gamma = TransferFunction::SRGB;
  else if (transferId == "Rec.709 (camera)" || transferId == "Rec.709") gamma = TransferFunction::Rec709;
  else if (transferId == "DaVinci Intermediate") gamma = TransferFunction::DaVinciIntermediate;
  else return false;

  encoding = {gamut, gamma};
  return true;
}

static ColorEncoding legacyOutputEncoding(int index) {
  switch (std::clamp(index, 0, 4)) {
    case 0: return {RgbGamut::Rec709, TransferFunction::SRGB};
    case 1: return {RgbGamut::DisplayP3, TransferFunction::SRGB};
    case 2: return {RgbGamut::Rec709, TransferFunction::Linear};
    case 3: return {RgbGamut::Rec2020, TransferFunction::Linear};
    case 4: return {RgbGamut::ACES_AP0, TransferFunction::Linear};
  }
  return {RgbGamut::Rec709, TransferFunction::SRGB};
}

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
  app.setStatus("RAW working encoding: " + colorEncodingName(requested));
}

void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma) {
  document_detail::setRawWorkingEncoding(app, gamut, gamma, {});
}

bool setOutputEncoding(App &app, ColorEncoding encoding) {
  if (!requireEditableGraph(app) || app.path.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.outputEncoding = encoding;
  }
  app.renderer.requestDisplayRefresh();
  return true;
}

bool captureGrade(App &app, PersistGrade &grade) {
  if (!requireEditableGraph(app)) return false;
  document_detail::DocumentMutation mutation(app);
  try {
    PersistGrade captured;
    captured.graph = captureGraph({app.nodes, app.primarySource, app.documentOutput});
    captured.color = captureGradeColor(app);
    captured.positions = app.nodePositions;
    std::string json, error;
    PersistFile file; file.grade = captured;
    if (!encodeGraphFile(file, json, error) || !decodeGraphFile(json, file, error)) throw std::runtime_error(error);
    grade = std::move(*file.grade);
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not capture grade: " + std::string(failure.what()));
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

bool document_detail::applyGrade(App &app, const PersistGrade &incoming, const ActionTestOptions &options) {
  if (!requireEditableGraph(app)) return false;
  document_detail::DocumentMutation mutation(app);
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
    auto restored = document_detail::restoreGraph(grade.graph);
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
      preview = document_detail::makePreviewSource(decoded, detected, app.previewRes);
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
    selectNode(app, std::nullopt);
    app.paramFilter[0] = '\0';
    app.renderer.resetInspection();
    mutation.changed();
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not apply full grade: " + std::string(failure.what()));
    return false;
  }
}

bool applyGrade(App &app, const PersistGrade &grade) {
  return document_detail::applyGrade(app, grade, {});
}

PersistGui captureGui(const App &app) {
  PersistGui g;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    g.outputColorSpace = rgbGamutId(app.defaultOutputEncoding.gamut);
    g.outputGamma = transferFunctionId(app.defaultOutputEncoding.gamma);
    g.rawDefaultColorSpace = rgbGamutId(app.rawWorkingEncoding.gamut);
    g.rawDefaultGamma = transferFunctionId(app.rawWorkingEncoding.gamma);
  }
  g.exportFormat = app.exportFormat;
  g.jpegQuality = app.jpegQuality;
  g.previewRes = app.previewRes;
  g.themeIndex = app.themeIndex;
  g.showLeft = app.showLeft;
  g.showRight = app.showRight;
  g.leftW = app.leftW;
  g.rightW = app.rightW;
  g.showFilmstrip = app.showFilmstrip;
  g.filmstripH = app.filmstripH;
  return g;
}

void applyGui(App &app, const PersistGui &g) {
  ColorEncoding output = legacyOutputEncoding(g.legacyOutputIndex);
  ColorEncoding persistedOutput;
  if (!g.outputColorSpace.empty() && !g.outputGamma.empty() &&
      persistedColorEncoding(g.outputColorSpace, g.outputGamma, persistedOutput))
    output = persistedOutput;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.defaultOutputEncoding = output;
    if (app.path.empty()) app.outputEncoding = output;
  }

  app.exportFormat = std::clamp(g.exportFormat, 0, 1);
  app.jpegQuality = std::clamp(g.jpegQuality, 1, 100);
  app.previewRes = std::clamp(g.previewRes, 0, kPreviewResCount - 1);
  if (g.themeIndex >= 0 && g.themeIndex < themeCount()) app.themeIndex = g.themeIndex;
  app.showLeft = g.showLeft;
  app.showRight = g.showRight;
  app.leftW = g.leftW;
  app.rightW = g.rightW;
  app.showFilmstrip = g.showFilmstrip;
  app.filmstripH = std::clamp(g.filmstripH, 48.0f, 240.0f);
  app.themeApplyPending = true;
}

static void applyWorkspaceSessionDefaults(App &app, const PersistGui &g) {
  ColorEncoding rawDefault;
  if (!g.rawDefaultColorSpace.empty() && !g.rawDefaultGamma.empty() &&
      persistedColorEncoding(g.rawDefaultColorSpace, g.rawDefaultGamma, rawDefault)) {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.rawWorkingEncoding = rawDefault;
  }
}

PersistGui captureSidecarGui(const App &app) {
  PersistGui g = captureGui(app);
  // RAW session defaults belong to workspace/app state, not to one image.
  g.rawDefaultColorSpace.clear();
  g.rawDefaultGamma.clear();
  return g;
}

namespace document_detail {
bool savePhoto(App &app, std::string &error, const ActionTestOptions &options) {
  error.clear();
  if (app.path.empty()) return true;
  if (app.isPhotoProtected()) {
    error = "The saved grade was not loaded; grade saving is disabled";
    app.setStatus(error);
    return false;
  }
  DocumentMutation mutation(app);
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

void persistWorkspace(App &app) {
  if (app.workspaceDir.empty() || app.workspaceWriteBlocked) return;
  const std::string active =
      app.path.empty() ? std::string() : relativeToWorkspace(app.workspaceDir, app.path);
  saveWorkspaceProject(app.workspaceDir, captureGui(app), active);
}

static bool hasUnknownEncodingPair(const std::string &gamutId, const std::string &transferId) {
  if (gamutId.empty() && transferId.empty()) return false;
  if (gamutId.empty() || transferId.empty()) return true;
  ColorEncoding encoding;
  return !persistedColorEncoding(gamutId, transferId, encoding);
}

static bool workspaceHasUnknownColourEncoding(const PersistGui &g) {
  return hasUnknownEncodingPair(g.outputColorSpace, g.outputGamma) ||
         hasUnknownEncodingPair(g.rawDefaultColorSpace, g.rawDefaultGamma);
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
    if (initialSave) { std::string failure; savePhoto(app, failure, options); }
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
  if (opened) persistWorkspace(app);
  return opened;
}

bool openFolder(App &app, const std::string &dir, const SaveFailurePrompt &prompt,
                const ActionTestOptions &options) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) { app.setStatus("Not a directory"); return false; }
  auto folder = fs::weakly_canonical(fs::path(dir), ec).string();
  if (ec) { app.setStatus("Could not open folder: " + ec.message()); return false; }
  if (!leavePhoto(app, prompt, options)) return false;
  persistWorkspace(app);
  app.workspaceDir.swap(folder);
  app.workspaceWriteBlocked = false;
  if (!ImGuiBackend_SetWorkspaceIni(app.workspaceDir)) app.layoutApplyPending = true;
  refreshFilmstrip(app);
  PersistGui gui;
  std::string active;
  if (loadWorkspaceProject(app.workspaceDir, gui, active)) {
    app.workspaceWriteBlocked = workspaceHasUnknownColourEncoding(gui);
    applyGui(app, gui);
    applyWorkspaceSessionDefaults(app, gui);
  }
  std::string toOpen;
  if (!active.empty()) {
    const auto candidate = fs::path(app.workspaceDir) / active;
    if (fs::is_regular_file(candidate, ec)) toOpen = candidate.string();
  }
  if (toOpen.empty() && !app.filmstrip.empty()) toOpen = app.filmstrip.front().path;
  if (!toOpen.empty()) openPreparedPhoto(app, toOpen, options);
  else app.setStatus("Folder: " + app.workspaceDir + " (no images)");
  persistWorkspace(app);
  return true;
}
}  // namespace document_detail

bool openWorkspace(App &app, const std::string &dir, const SaveFailurePrompt &prompt) {
  return document_detail::openFolder(app, dir, prompt, {});
}
bool openPath(App &app, const std::string &path, const SaveFailurePrompt &prompt) {
  return document_detail::openPhoto(app, path, prompt, {});
}
