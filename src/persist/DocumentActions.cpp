#include "persist/DocumentActions.h"

#include "ui/Filmstrip.h"
#include "imgio/ImageIO.h"
#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"
#include "ui/Themes.h"
#include "ui/ImGuiBackend.h"

#include "portable-file-dialogs.h"

#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

static ColorEncoding legacyOutputEncoding(int index) {
  return legacyColorSpaceEncoding((ColorSpace)std::clamp(index, 0, 4));
}

static int legacyOutputIndex(const ColorEncoding &encoding) {
  ColorSpace legacy;
  return legacyColorSpaceFromEncoding(encoding, legacy) ? (int)legacy : 0;
}

PersistGui captureGui(const App &app) {
  PersistGui g;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    g.outputIndex = legacyOutputIndex(app.outputEncoding);
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
  RgbGamut gamut;
  TransferFunction gamma;
  ColorEncoding output = legacyOutputEncoding(g.outputIndex);
  if (!g.outputColorSpace.empty() && !g.outputGamma.empty() &&
      rgbGamutFromIdOrName(g.outputColorSpace, gamut) &&
      transferFunctionFromIdOrName(g.outputGamma, gamma))
    output = {gamut, gamma};
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
  RgbGamut rawGamut;
  TransferFunction rawGamma;
  if (!g.rawDefaultColorSpace.empty() && !g.rawDefaultGamma.empty() &&
      rgbGamutFromIdOrName(g.rawDefaultColorSpace, rawGamut) &&
      transferFunctionFromIdOrName(g.rawDefaultGamma, rawGamma)) {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.rawWorkingEncoding = {rawGamut, rawGamma};
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
  saveInputSidecar(app.path, inputEncoding, captureSidecarGui(app), captureChain(app),
                   inputIsRaw ? &inputEncoding : nullptr);
}

void persistWorkspace(App &app) {
  if (app.workspaceDir.empty() || app.workspaceWriteBlocked) return;
  const std::string active =
      app.path.empty() ? std::string() : relativeToWorkspace(app.workspaceDir, app.path);
  saveWorkspaceProject(app.workspaceDir, captureGui(app), active);
}

static bool sidecarHasUnknownColourEncoding(const PersistSidecar &sc) {
  if (!sc.rawColorSpace.empty() || !sc.rawGamma.empty()) {
    RgbGamut gamut;
    TransferFunction gamma;
    if (sc.rawColorSpace.empty() || sc.rawGamma.empty() ||
        !rgbGamutFromIdOrName(sc.rawColorSpace, gamut) ||
        !transferFunctionFromIdOrName(sc.rawGamma, gamma))
      return true;
  }

  if (!sc.gui.outputColorSpace.empty() || !sc.gui.outputGamma.empty()) {
    RgbGamut gamut;
    TransferFunction gamma;
    if (sc.gui.outputColorSpace.empty() || sc.gui.outputGamma.empty() ||
        !rgbGamutFromIdOrName(sc.gui.outputColorSpace, gamut) ||
        !transferFunctionFromIdOrName(sc.gui.outputGamma, gamma))
      return true;
  }

  // Older development builds accidentally wrote the RAW session default into
  // per-image sidecars. Ignore recognised values, but do not destroy an
  // unknown future value if one is present.
  if (!sc.gui.rawDefaultColorSpace.empty() || !sc.gui.rawDefaultGamma.empty()) {
    RgbGamut gamut;
    TransferFunction gamma;
    if (sc.gui.rawDefaultColorSpace.empty() || sc.gui.rawDefaultGamma.empty() ||
        !rgbGamutFromIdOrName(sc.gui.rawDefaultColorSpace, gamut) ||
        !transferFunctionFromIdOrName(sc.gui.rawDefaultGamma, gamma))
      return true;
  }
  return false;
}

static void loadSidecarForPath(App &app, const std::string &imagePath) {
  PersistSidecar sc;
  const std::string v2Path = inputSidecarPath(imagePath);
  const std::string v1Path = legacyInputSidecarPath(imagePath);
  std::error_code ec;

  app.sidecarWriteBlockedPath.clear();
  app.sidecarBlockedByUnknownProcessorChoice = false;

  if (fs::is_regular_file(v2Path, ec)) {
    if (!loadSidecarFile(v2Path, sc)) {
      clearNodes(app);
      app.sidecarWriteBlockedPath = imagePath;
      app.sidecarBlockedByUnknownProcessorChoice = false;
      if (sc.format == "rawnode-sidecar" && sc.version > 2) {
        app.setStatus("This image uses a newer RawNode sidecar version; edits are not being overwritten.");
      } else {
        app.setStatus("Could not read RawNode sidecar; the existing file is protected from overwrite.");
      }
      return;
    }
  } else if (fs::is_regular_file(v1Path, ec)) {
    if (!loadSidecarFile(v1Path, sc)) {
      clearNodes(app);
      app.sidecarWriteBlockedPath = imagePath;
      app.sidecarBlockedByUnknownProcessorChoice = false;
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
    app.sidecarBlockedByUnknownProcessorChoice = false;
    app.setStatus("This sidecar contains colour settings this version of RawNode does not recognise; edits are not being overwritten.");
    return;
  }

  if (hasUnknownProcessorChoiceIds(app)) {
    app.sidecarWriteBlockedPath = imagePath;
    app.sidecarBlockedByUnknownProcessorChoice = true;
    app.setStatus("This sidecar contains processor choices this version of RawNode does not recognise. They are not being applied, and the sidecar is protected until you replace them.");
  }
}

static bool workspaceHasUnknownColourEncoding(const PersistGui &g) {
  RgbGamut gamut;
  TransferFunction gamma;

  if (!g.outputColorSpace.empty() || !g.outputGamma.empty()) {
    if (g.outputColorSpace.empty() || g.outputGamma.empty() ||
        !rgbGamutFromIdOrName(g.outputColorSpace, gamut) ||
        !transferFunctionFromIdOrName(g.outputGamma, gamma))
      return true;
  }

  if (!g.rawDefaultColorSpace.empty() || !g.rawDefaultGamma.empty()) {
    if (g.rawDefaultColorSpace.empty() || g.rawDefaultGamma.empty() ||
        !rgbGamutFromIdOrName(g.rawDefaultColorSpace, gamut) ||
        !transferFunctionFromIdOrName(g.rawDefaultGamma, gamma))
      return true;
  }

  return false;
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
      RgbGamut gamut;
      TransferFunction gamma;
      if (!sc.rawColorSpace.empty() && !sc.rawGamma.empty() &&
          rgbGamutFromIdOrName(sc.rawColorSpace, gamut) &&
          transferFunctionFromIdOrName(sc.rawGamma, gamma))
        return {gamut, gamma};

      // Unknown future explicit encoding. Decode with the historical safe
      // fallback; loadSidecarForPath will write-protect the sidecar.
      return {RgbGamut::Rec709, TransferFunction::Linear};
    }

    ColorSpace stored;
    if (!sc.rawWorkingSpace.empty() &&
        colorSpaceFromName(sc.rawWorkingSpace, stored) &&
        isRawWorkingSpace(stored))
      return legacyColorSpaceEncoding(stored);

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
    app.sidecarBlockedByUnknownProcessorChoice = false;
  }

  rebuildPreview(app);
  persistWorkspace(app);
}

void setRawWorkingEncoding(App &app, RgbGamut gamut, TransferFunction gamma) {
  const ColorEncoding requested{gamut, gamma};
  ColorEncoding current;
  bool currentIsRaw = false;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    currentIsRaw = app.inputIsRaw;
    current = currentIsRaw ? app.inputEncoding : app.rawWorkingEncoding;
    // An explicit UI choice also becomes the session default for new RAWs.
    app.rawWorkingEncoding = requested;
  }
  if (current == requested) return;

  // For raster images this is simply the preference for the next RAW.
  if (!currentIsRaw) return;

  waitRenderIdle(app);
  Image img;
  ColorEncoding detectedEncoding;
  bool decodedRaw = false;
  if (!loadImage(app.path, img, detectedEncoding, decodedRaw, requested) || !decodedRaw) {
    app.setStatus("Could not reload RAW in " + colorEncodingName(requested));
    return;
  }

  app.full = std::move(img);
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    app.inputEncoding = detectedEncoding;
    app.inputIsRaw = true;
  }
  rebuildPreview(app);
  saveCurrentInputSidecar(app);
  persistWorkspace(app);
  app.setStatus("RAW working encoding: " + colorEncodingName(requested));
}

void setRawWorkingSpace(App &app, ColorSpace space) {
  if (!isRawWorkingSpace(space)) return;
  const ColorEncoding encoding = legacyColorSpaceEncoding(space);
  setRawWorkingEncoding(app, encoding.gamut, encoding.gamma);
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

  app.setStatus("Exporting full resolution...");
  waitRenderIdle(app);
  const int pw = app.preview.w, ph = app.preview.h;
  Image src = app.full;
  ColorEncoding space;
  ColorEncoding inSpace;
  bool sourceUsesRawEncoding = false;
  ColorEncoding sourceRawEncoding;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    space = app.outputEncoding;
    inSpace = app.inputEncoding;
    sourceUsesRawEncoding = app.inputIsRaw;
    sourceRawEncoding = app.inputEncoding;
  }
  const int jpegQuality = app.jpegQuality;
  const PersistGui persistGui = captureSidecarGui(app);
  const PersistChain persistChain = captureChain(app);
  const std::string sourcePath = app.path;
  bool bypassedMissingProcessor = false;
  for (const auto &node : app.nodes) {
    if (node.enabled && !node.processor) {
      bypassedMissingProcessor = true;
      break;
    }
  }
  std::thread([&, src, outPath, pw, ph, space, jpegQuality, persistGui, persistChain, sourcePath, inSpace,
               sourceUsesRawEncoding, sourceRawEncoding, bypassedMissingProcessor]() mutable {
    for (auto &n : app.nodes)
      if (n.processor) n.processor->setRenderSize(src.w, src.h);
    Image out;
    ProcessorResult result = renderChain(app, src, out, 0);
    for (auto &n : app.nodes)
      if (n.processor) n.processor->setRenderSize(pw, ph);
    bool ok = result.ok && writeImage(out, outPath, space, jpegQuality);
    if (ok) saveExportSidecar(outPath, sourcePath, inSpace, persistGui, persistChain,
                              sourceUsesRawEncoding ? &sourceRawEncoding : nullptr);
    if (ok) {
      std::string status = "Exported " + fs::path(outPath).filename().string() + " (" +
                           std::to_string(src.w) + "×" + std::to_string(src.h) + ")";
      if (bypassedMissingProcessor) status += " — missing processors were bypassed";
      if (space.gamma == TransferFunction::DaVinciIntermediate)
        status += " — warning: ICC cannot fully represent DaVinci Intermediate scene values above 1.0; external apps may clip highlights";
      app.setStatus(status);
    } else {
      app.setStatus("Export failed" + (result.message.empty() ? std::string() : ": " + result.message));
    }
  }).detach();
}
