#include "persist/DocumentActions.h"

#include "ui/Filmstrip.h"
#include "imgio/ImageIO.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "NodeGraph.h"
#include "DocumentMutation.h"
#include "RenderPipeline.h"
#include "ui/Themes.h"
#include "ui/ImGuiBackend.h"

#include "portable-file-dialogs.h"

#include <exception>
#include <filesystem>

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

static bool legacyRawEncodingFromName(const std::string &name, ColorEncoding &encoding) {
  if (name == "Linear Rec.709") {
    encoding = {RgbGamut::Rec709, TransferFunction::Linear};
    return true;
  }
  if (name == "Linear Rec.2020") {
    encoding = {RgbGamut::Rec2020, TransferFunction::Linear};
    return true;
  }
  if (name == "ACES2065-1" || name == "ACES2065-1 (AP0)" || name == "AP0") {
    encoding = {RgbGamut::ACES_AP0, TransferFunction::Linear};
    return true;
  }
  return false;
}

PersistGradeColor captureGradeColor(const App &app) {
  PersistGradeColor color;
  std::lock_guard<std::mutex> lock(app.colorMutex);
  if (app.inputIsRaw) {
    color.rawColorSpace = rgbGamutId(app.inputEncoding.gamut);
    color.rawGamma = transferFunctionId(app.inputEncoding.gamma);
  }
  color.outputColorSpace = rgbGamutId(app.outputEncoding.gamut);
  color.outputGamma = transferFunctionId(app.outputEncoding.gamma);
  return color;
}

static bool reloadCurrentRawEncoding(App &app, const ColorEncoding &requested,
                                     bool updateSessionDefault, bool persistAfter) {
  ColorEncoding current;
  bool currentIsRaw = false;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    currentIsRaw = app.inputIsRaw;
    current = currentIsRaw ? app.inputEncoding : app.rawWorkingEncoding;
    // Defaults may change immediately when no source reload is needed. A RAW
    // reload commits both source and default only after decoding succeeds.
    if (updateSessionDefault && (!currentIsRaw || current == requested))
      app.rawWorkingEncoding = requested;
  }

  if (!currentIsRaw) return true;
  if (current == requested) return true;

  document_detail::DocumentMutation mutation(app);
  Image img;
  ColorEncoding detectedEncoding;
  bool decodedRaw = false;
  if (!loadImage(app.path, img, detectedEncoding, decodedRaw, requested) || !decodedRaw) {
    app.setStatus("Could not reload RAW in " + colorEncodingName(requested));
    return false;
  }

  app.full = std::move(img);
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.inputEncoding = detectedEncoding;
    app.inputIsRaw = true;
    if (updateSessionDefault) app.rawWorkingEncoding = requested;
  }
  mutation.changed();
  mutation.rebuildPreview();
  if (persistAfter) {
    saveCurrentInputSidecar(app);
    persistWorkspace(app);
  }
  return true;
}

bool applyGradeColor(App &app, const PersistGradeColor &color) {
  ColorEncoding output;
  if (!color.outputColorSpace.empty() || !color.outputGamma.empty()) {
    if (color.outputColorSpace.empty() || color.outputGamma.empty() ||
        !persistedColorEncoding(color.outputColorSpace, color.outputGamma, output))
      return false;
  }

  ColorEncoding raw;
  const bool hasRaw = !color.rawColorSpace.empty() || !color.rawGamma.empty();
  if (hasRaw) {
    if (color.rawColorSpace.empty() || color.rawGamma.empty() ||
        !persistedColorEncoding(color.rawColorSpace, color.rawGamma, raw))
      return false;

    bool currentIsRaw = false;
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      currentIsRaw = app.inputIsRaw;
    }
    if (currentIsRaw && !reloadCurrentRawEncoding(app, raw, false, false))
      return false;
  }

  if (!color.outputColorSpace.empty()) {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.outputEncoding = output;
  }
  return true;
}

PersistGui captureGui(const App &app) {
  PersistGui g;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    g.outputColorSpace = rgbGamutId(app.outputEncoding.gamut);
    g.outputGamma = transferFunctionId(app.outputEncoding.gamma);
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
    app.outputEncoding = output;
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

static PersistGui captureSidecarGui(const App &app) {
  PersistGui g = captureGui(app);
  // RAW session defaults belong to workspace/app state, not to one image.
  g.rawDefaultColorSpace.clear();
  g.rawDefaultGamma.clear();
  return g;
}

void saveCurrentInputSidecar(App &app) {
  if (app.path.empty()) return;
  if (app.sidecarWriteBlockedPath == app.path) return;
  ColorEncoding inputEncoding;
  bool inputIsRaw = false;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    inputEncoding = app.inputEncoding;
    inputIsRaw = app.inputIsRaw;
  }
  saveInputSidecar(app.path, captureSidecarGui(app), captureChain(app),
                   inputIsRaw ? &inputEncoding : nullptr);
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

static bool sidecarHasUnknownColourEncoding(const PersistSidecar &sc) {
  if (hasUnknownEncodingPair(sc.rawColorSpace, sc.rawGamma)) return true;
  if (hasUnknownEncodingPair(sc.gui.outputColorSpace, sc.gui.outputGamma)) return true;

  // Older development builds accidentally wrote the RAW session default into
  // per-image sidecars. Ignore recognised values, but protect unknown values.
  return hasUnknownEncodingPair(sc.gui.rawDefaultColorSpace, sc.gui.rawDefaultGamma);
}

static void loadSidecarForPath(App &app, const std::string &imagePath) {
  PersistSidecar sc;
  const std::string v2Path = inputSidecarPath(imagePath);
  const std::string v1Path = legacyInputSidecarPath(imagePath);
  std::error_code ec;

  app.sidecarWriteBlockedPath.clear();

  if (fs::is_regular_file(v2Path, ec)) {
    if (!loadSidecarFile(v2Path, sc)) {
      clearNodes(app);
      app.sidecarWriteBlockedPath = imagePath;
      if (sc.format == "rawnode-sidecar" && sc.version > 2) {
        app.setStatus("This image uses a newer RawNode sidecar version; changes will not be saved.");
      } else {
        app.setStatus("Could not read RawNode sidecar; the existing file is protected from overwrite.");
      }
      return;
    }
  } else if (fs::is_regular_file(v1Path, ec)) {
    if (!loadSidecarFile(v1Path, sc)) {
      clearNodes(app);
      app.sidecarWriteBlockedPath = imagePath;
      app.setStatus("Could not read legacy sidecar; the existing file is protected from overwrite.");
      return;
    }
  } else {
    clearNodes(app);
    return;
  }

  applyGui(app, sc.gui);
  applyChain(app, sc.chain);

  // Unknown future colour identifiers must never be silently replaced by this
  // build's fallback values on the next automatic save.
  if (sidecarHasUnknownColourEncoding(sc)) {
    app.sidecarWriteBlockedPath = imagePath;
    app.setStatus("This sidecar contains colour settings this version of RawNode does not recognise; changes will not be saved.");
    return;
  }

  if (hasUnknownProcessorChoiceIds(app)) {
    app.sidecarWriteBlockedPath = imagePath;
    app.setStatus("This sidecar contains processor settings this version of RawNode does not recognise; changes will not be saved.");
  }
}

static bool workspaceHasUnknownColourEncoding(const PersistGui &g) {
  return hasUnknownEncodingPair(g.outputColorSpace, g.outputGamma) ||
         hasUnknownEncodingPair(g.rawDefaultColorSpace, g.rawDefaultGamma);
}

void openWorkspace(App &app, const std::string &dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    app.setStatus("Not a directory");
    return;
  }
  saveCurrentInputSidecar(app);
  persistWorkspace(app);
  app.workspaceDir = fs::weakly_canonical(fs::path(dir), ec).string();
  app.workspaceWriteBlocked = false;
  if (!ImGuiBackend_SetWorkspaceIni(app.workspaceDir)) app.layoutApplyPending = true;
  refreshFilmstrip(app);
  PersistGui wg;
  std::string activeRel;
  if (loadWorkspaceProject(app.workspaceDir, wg, activeRel)) {
    if (workspaceHasUnknownColourEncoding(wg)) {
      app.workspaceWriteBlocked = true;
      app.setStatus("This workspace contains colour settings this version of RawNode does not recognise; the workspace file is protected from overwrite.");
    }
    applyGui(app, wg);
    applyWorkspaceSessionDefaults(app, wg);
  }
  std::string toOpen;
  if (!activeRel.empty()) {
    fs::path p = fs::path(app.workspaceDir) / activeRel;
    if (fs::is_regular_file(p, ec)) toOpen = p.string();
  }
  if (toOpen.empty() && !app.filmstrip.empty()) toOpen = app.filmstrip[0].path;
  if (!toOpen.empty())
    openPath(app, toOpen, true);
  else
    app.setStatus("Workspace: " + fs::path(app.workspaceDir).filename().string() + " (no images)");

  if (app.workspaceWriteBlocked)
    app.setStatus("This workspace contains colour settings this version of RawNode does not recognise; the workspace file is protected from overwrite.");
  persistWorkspace(app);
}

static ColorEncoding rawWorkingEncodingForOpen(const App &app, const std::string &path,
                                               bool applySidecar) {
  ColorEncoding session;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    session = app.rawWorkingEncoding;
  }
  if (!applySidecar) return session;

  std::error_code ec;
  const std::string v2Path = inputSidecarPath(path);
  const std::string v1Path = legacyInputSidecarPath(path);
  PersistSidecar sc;

  if (fs::is_regular_file(v2Path, ec)) {
    if (!loadSidecarFile(v2Path, sc))
      return {RgbGamut::Rec709, TransferFunction::Linear};

    if (!sc.rawColorSpace.empty() || !sc.rawGamma.empty()) {
      ColorEncoding stored;
      if (!sc.rawColorSpace.empty() && !sc.rawGamma.empty() &&
          persistedColorEncoding(sc.rawColorSpace, sc.rawGamma, stored))
        return stored;

      // Unknown future explicit encoding. Decode with the historical safe
      // fallback; loadSidecarForPath will write-protect the sidecar.
      return {RgbGamut::Rec709, TransferFunction::Linear};
    }

    ColorEncoding stored;
    if (!sc.legacyRawWorkingSpace.empty() &&
        legacyRawEncodingFromName(sc.legacyRawWorkingSpace, stored))
      return stored;

    // V2 sidecar predating selectable RAW working space.
    return {RgbGamut::Rec709, TransferFunction::Linear};
  }

  if (fs::is_regular_file(v1Path, ec))
    return {RgbGamut::Rec709, TransferFunction::Linear};

  return session;
}

void openPath(App &app, const std::string &path, bool applySidecar) {
  if (isHostMetadataPath(path)) {
    app.setStatus("Sidecar files are not images — open the image file instead.");
    return;
  }
  if (!app.path.empty() && app.path != path) saveCurrentInputSidecar(app);

  const ColorEncoding requestedRaw = rawWorkingEncodingForOpen(app, path, applySidecar);
  Image img;
  ColorEncoding detectedEncoding;
  bool decodedRaw = false;
  if (!loadImage(path, img, detectedEncoding, decodedRaw, requestedRaw)) {
    app.setStatus("Could not decode " + fs::path(path).filename().string());
    return;
  }

  app.path = path;
  app.full = std::move(img);
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.inputEncoding = detectedEncoding;
    app.inputIsRaw = decodedRaw;
  }
  app.previewZoom = 1.0f;
  app.previewPanX = 0.0f;
  app.previewPanY = 0.0f;
  app.setStatus("Loaded " + fs::path(path).filename().string() + " (" +
                colorEncodingName(detectedEncoding) + ")");

  app.filmstripIndex = -1;
  for (int i = 0; i < (int)app.filmstrip.size(); ++i) {
    std::error_code ec;
    if (fs::equivalent(app.filmstrip[i].path, path, ec)) {
      app.filmstripIndex = i;
      break;
    }
  }

  if (applySidecar) {
    loadSidecarForPath(app, path);
  } else {
    app.sidecarWriteBlockedPath.clear();
  }

  rebuildPreview(app);
  persistWorkspace(app);
}

void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma) {
  const ColorEncoding requested{gamut, gamma};
  if (!reloadCurrentRawEncoding(app, requested, true, true)) {
    app.setStatus("Could not reload RAW in " + colorEncodingName(requested));
    return;
  }
  app.setStatus("RAW working encoding: " + colorEncodingName(requested));
}

namespace {

struct ExportRequest {
  Image source;
  std::string outPath, sourcePath;
  int previewWidth = 0, previewHeight = 0;
  ColorEncoding space, sourceRawEncoding;
  bool sourceUsesRawEncoding = false;
  bool bypassedMissingProcessor = false;
  int jpegQuality = 92;
  PersistGui gui;
  PersistChain chain;
};

static std::string exceptionMessage(std::exception_ptr error) {
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &e) {
    return e.what();
  } catch (...) {
    return "Unknown exception";
  }
}

static void publishExportException(App &app, std::exception_ptr error,
                                   std::exception_ptr restorationError = {}) noexcept {
  try {
    std::string status = "Export failed: " + exceptionMessage(error);
    if (restorationError && restorationError != error)
      status += " (preview sizing: " + exceptionMessage(restorationError) + ")";
    app.setStatus(status);
  } catch (...) {
    // Even an allocation failure while reporting an error must not escape the
    // thread entry point or prevent ownership release.
    try { app.setStatus("Export failed"); } catch (...) {}
  }
}

struct PreviewSizeRestore {
  App &app;
  int width, height;
  std::exception_ptr &error;
  ~PreviewSizeRestore() {
    // Attempt every processor, including one whose full-size setter threw
    // after changing its state. Cleanup itself must not interrupt unwinding.
    for (auto &node : app.nodes) {
      if (!node.processor) continue;
      try {
        node.processor->setRenderSize(width, height);
      } catch (...) {
        if (!error) error = std::current_exception();
      }
    }
  }
};

static ExportRequest captureExportRequest(App &app, const std::string &outPath) {
  ExportRequest request;
  request.outPath = outPath;
  request.source = app.full;
  request.previewWidth = app.preview.w;
  request.previewHeight = app.preview.h;
  request.sourcePath = app.path;
  request.jpegQuality = app.jpegQuality;
  request.gui = captureSidecarGui(app);
  request.chain = captureChain(app);
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    request.space = app.outputEncoding;
    request.sourceUsesRawEncoding = app.inputIsRaw;
    request.sourceRawEncoding = app.inputEncoding;
  }
  for (const auto &node : app.nodes)
    if (node.enabled && !node.processor) request.bypassedMissingProcessor = true;
  return request;
}

// This is the production export body for both synchronous and UI execution.
// The caller retains full-resolution ownership through final status publication.
static bool executeExportJob(App &app, const ExportRequest &request) noexcept {
  std::exception_ptr restorationError;
  try {
    Image out;
    ProcessorResult result;
    {
      PreviewSizeRestore restore{app, request.previewWidth, request.previewHeight, restorationError};
      for (auto &node : app.nodes)
        if (node.processor) node.processor->setRenderSize(request.source.w, request.source.h);
      result = renderChain(app, request.source, out);
    }
    if (restorationError) std::rethrow_exception(restorationError);
    const bool ok = result.ok && writeImage(out, request.outPath, request.space, request.jpegQuality);
    if (ok) saveExportSidecar(request.outPath, request.sourcePath, request.gui, request.chain,
                              request.sourceUsesRawEncoding ? &request.sourceRawEncoding : nullptr);
    if (ok) {
      std::string status = "Exported " + fs::path(request.outPath).filename().string() + " (" +
                           std::to_string(request.source.w) + "×" + std::to_string(request.source.h) + ")";
      if (request.bypassedMissingProcessor) status += " — missing processors were bypassed";
      if (request.space.gamma == TransferFunction::DaVinciIntermediate)
        status += " — warning: ICC cannot fully represent DaVinci Intermediate scene values above 1.0; external apps may clip highlights";
      app.setStatus(status);
    } else {
      app.setStatus("Export failed" + (result.message.empty() ? std::string() : ": " + result.message));
    }
    return ok;
  } catch (...) {
    publishExportException(app, std::current_exception(), restorationError);
    return false;
  }
}

}  // namespace

bool runExportJob(App &app, const std::string &outPath) {
  if (app.full.px.empty() || app.nodes.empty() || outPath.empty()) return false;
  auto ownership = app.renderer.acquireExport();
  if (!ownership) return false;
  try {
    app.setStatus("Exporting full resolution...");
    return executeExportJob(app, captureExportRequest(app, outPath));
  } catch (...) {
    publishExportException(app, std::current_exception());
    return false;
  }
}

bool startExport(App &app, const std::string &outPath) {
  if (app.full.px.empty() || app.nodes.empty() || outPath.empty()) return false;
  auto ownership = app.renderer.acquireExport();
  if (!ownership) return false;
  try {
    app.setStatus("Exporting full resolution...");
    ExportRequest request = captureExportRequest(app, outPath);
    ownership.start([&app, request = std::move(request)]() noexcept {
      executeExportJob(app, request);
    });
    return true;
  } catch (...) {
    publishExportException(app, std::current_exception());
    return false;
  }
}

void doExport(App &app) {
  if (app.full.px.empty() || app.nodes.empty()) return;
  const char *exts[] = {".png", ".jpg"};
  const char *filters[] = {"PNG (8-bit)", "*.png", "JPEG", "*.jpg *.jpeg"};
  std::string def = fs::path(app.path).stem().string() + exts[app.exportFormat];
  auto sel = pfd::save_file("Export", def, {filters[app.exportFormat * 2], filters[app.exportFormat * 2 + 1]});
  std::string outPath = sel.result();
  if (outPath.empty()) return;
  if (fs::path(outPath).extension().empty()) outPath += exts[app.exportFormat];
  startExport(app, outPath);
}
