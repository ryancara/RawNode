#include "persist/DocumentActions.h"

#include "ui/Filmstrip.h"
#include "imgio/ImageIO.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"
#include "ui/Themes.h"
#include "ui/ImGuiBackend.h"

#include "portable-file-dialogs.h"

#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

PersistGui captureGui(const App &app) {
  PersistGui g;
  g.outputIndex = app.outputIndex;
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
  app.outputIndex = std::clamp(g.outputIndex, 0, kOutputSpaceCount - 1);
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

void saveCurrentInputSidecar(App &app) {
  if (app.path.empty()) return;
  if (app.sidecarWriteBlockedPath == app.path) return;
  saveInputSidecar(app.path, app.inputSpace, captureGui(app), captureChain(app));
}

void persistWorkspace(App &app) {
  if (app.workspaceDir.empty()) return;
  const std::string active =
      app.path.empty() ? std::string() : relativeToWorkspace(app.workspaceDir, app.path);
  saveWorkspaceProject(app.workspaceDir, captureGui(app), active);
}

static void loadSidecarForPath(App &app, const std::string &imagePath) {
  PersistSidecar sc;
  const std::string v2Path = inputSidecarPath(imagePath);
  const std::string v1Path = legacyInputSidecarPath(imagePath);
  std::error_code ec;

  // A successful retry, a removed sidecar, or moving to another image clears
  // any previous write protection for this document.
  app.sidecarWriteBlockedPath.clear();

  if (fs::is_regular_file(v2Path, ec)) {
    if (!loadSidecarFile(v2Path, sc)) {
      clearNodes(app);
      app.sidecarWriteBlockedPath = imagePath;
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
      app.setStatus("Could not read legacy sidecar; the existing file is protected from overwrite.");
      return;
    }
  } else {
    // A new image with no sidecar starts with a clean processing chain.
    // Never inherit the previously opened image's nodes into this document.
    clearNodes(app);
    return;
  }

  applyGui(app, sc.gui);
  applyChain(app, sc.chain);
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
  if (!ImGuiBackend_SetWorkspaceIni(app.workspaceDir)) app.layoutApplyPending = true;
  refreshFilmstrip(app);
  PersistGui wg;
  std::string activeRel;
  if (loadWorkspaceProject(app.workspaceDir, wg, activeRel)) applyGui(app, wg);
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
  persistWorkspace(app);
}

static ColorSpace rawWorkingSpaceForOpen(const App &app, const std::string &path, bool applySidecar) {
  if (!isRawImagePath(path)) return app.rawWorkingSpace;

  // New images use the current/session preference. Existing sidecars created
  // before this feature have no raw.workingSpace field; those intentionally
  // reopen as Linear Rec.709 so previously saved edits retain PR #15 semantics.
  ColorSpace requested = app.rawWorkingSpace;
  if (!applySidecar) return requested;

  std::error_code ec;
  const std::string v2Path = inputSidecarPath(path);
  const std::string v1Path = legacyInputSidecarPath(path);
  PersistSidecar sc;
  if (fs::is_regular_file(v2Path, ec)) {
    if (!loadSidecarFile(v2Path, sc)) return ColorSpace::LinearRec709;
    ColorSpace stored;
    if (!sc.rawWorkingSpace.empty() && colorSpaceFromName(sc.rawWorkingSpace, stored) && isRawWorkingSpace(stored))
      return stored;
    return ColorSpace::LinearRec709;
  }
  if (fs::is_regular_file(v1Path, ec)) return ColorSpace::LinearRec709;
  return requested;
}

void openPath(App &app, const std::string &path, bool applySidecar) {
  if (isHostMetadataPath(path)) {
    app.setStatus("Sidecar files are not images — open the image file instead.");
    return;
  }
  if (!app.path.empty() && app.path != path) saveCurrentInputSidecar(app);

  const ColorSpace requestedRawSpace = rawWorkingSpaceForOpen(app, path, applySidecar);
  Image img;
  ColorSpace detected = ColorSpace::LinearRec2020;
  if (!loadImage(path, img, detected, requestedRawSpace)) {
    app.setStatus("Could not decode " + fs::path(path).filename().string());
    return;
  }
  app.path = path;
  app.full = std::move(img);
  app.inputSpace = detected;
  app.previewZoom = 1.0f;
  app.previewPanX = 0.0f;
  app.previewPanY = 0.0f;
  app.setStatus("Loaded " + fs::path(path).filename().string() + " (" + colorSpaceName(detected) + ")");
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
    for (auto &node : app.nodes) applyColorDefaults(app, node);
  }
  rebuildPreview(app);
  persistWorkspace(app);
}

void setRawWorkingSpace(App &app, ColorSpace space) {
  if (!isRawWorkingSpace(space) || app.rawWorkingSpace == space) return;
  app.rawWorkingSpace = space;

  // For raster images this is simply the preference for the next RAW.
  if (app.path.empty() || !isRawImagePath(app.path)) return;

  waitRenderIdle(app);
  Image img;
  ColorSpace detected = space;
  if (!loadImage(app.path, img, detected, space)) {
    app.setStatus("Could not reload RAW in " + std::string(colorSpaceName(space)));
    return;
  }

  app.full = std::move(img);
  app.inputSpace = detected;
  rebuildPreview(app);
  saveCurrentInputSidecar(app);
  persistWorkspace(app);
  app.setStatus("RAW working space: " + std::string(colorSpaceName(detected)));
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
  const ColorSpace space = outputSpace(app.outputIndex);
  const ColorSpace inSpace = app.inputSpace;
  const int jpegQuality = app.jpegQuality;
  const PersistGui persistGui = captureGui(app);
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
               bypassedMissingProcessor]() mutable {
    for (auto &n : app.nodes)
      if (n.processor) n.processor->setRenderSize(src.w, src.h);
    Image out;
    ProcessorResult result = renderChain(app, src, out, 0);
    for (auto &n : app.nodes)
      if (n.processor) n.processor->setRenderSize(pw, ph);
    bool ok = result.ok && writeImage(out, outPath, space, jpegQuality);
    if (ok) saveExportSidecar(outPath, sourcePath, inSpace, persistGui, persistChain);
    app.setStatus(ok ? "Exported " + fs::path(outPath).filename().string() + " (" + std::to_string(src.w) + "×" +
                            std::to_string(src.h) + ")" +
                            (bypassedMissingProcessor ? " — missing processors were bypassed" : "")
                      : "Export failed" + (result.message.empty() ? std::string() : ": " + result.message));
  }).detach();
}
