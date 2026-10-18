#include "Export.h"

#include "AppState.h"

#include <exception>
#include <filesystem>

namespace fs = std::filesystem;
namespace {
struct ExportRequest {
  ImageValue source;
  OutputRef output;
  std::string outPath;
  EvaluationContext context;
  ColorEncoding space;
  int jpegQuality;
};
static bool refuseActiveSourceOverwrite(App &app, const std::string &outPath) {
  if (app.path.empty()) return false;
  // Called on the control thread under the export lease, before evaluation or
  // file opening. equivalent resolves relative paths and symlink/hardlink aliases;
  // a nonexistent destination remains available for ordinary export.
  std::error_code error;
  if (!fs::equivalent(app.path, outPath, error)) return false;
  app.setStatus("Export refused: Cannot overwrite the currently opened source photograph");
  return true;
}
static ExportRequest captureExportRequest(App &app, const std::string &path) {
  ColorEncoding space;
  { std::lock_guard<std::mutex> lock(app.colorMutex); space = app.outputEncoding; }
  // The export lease drains/blocks mutation and outlives evaluation/file writing.
  // Borrow full Source under that ownership rather than copying a full raster.
  return {ImageValue(&app.full, [](const Image *) {}), app.documentOutput, path,
          {EvaluationPurpose::Export, app.full.w, app.full.h, app.full.w, app.full.h}, space, app.jpegQuality};
}
static void publishExportException(App &app, std::exception_ptr error) noexcept {
  try {
    std::string message = "Unknown exception";
    try { std::rethrow_exception(error); }
    catch (const std::exception &e) { message = e.what(); }
    catch (...) {}
    app.setStatus("Export failed: " + message);
  } catch (...) {
    try { app.setStatus("Export failed"); } catch (...) {}
  }
}
static bool executeExportJob(App &app, const ExportRequest &request) noexcept {
  try {
    auto result = app.renderer.evaluateOwned(request.output, request.context, request.source);
    const auto *image = std::get_if<ImageValue>(&result.value);
    if (result.state != EvaluationState::Available || !image || !*image) {
      std::string status = result.state == EvaluationState::Cancelled ? "Export cancelled" : "Export failed";
      if (result.state == EvaluationState::Unavailable) status += ": Document output unavailable";
      for (const auto &diagnostic : result.diagnostics) status += " — " + diagnostic.message;
      app.setStatus(status);
      return false;
    }
    if (!writeImage(**image, request.outPath, request.space, request.jpegQuality)) {
      app.setStatus("Export failed: Could not write image");
      return false;
    }
    std::string status = "Exported " + fs::path(request.outPath).filename().string() + " (" +
                         std::to_string((*image)->w) + "×" + std::to_string((*image)->h) + ")";
    for (const auto &diagnostic : result.diagnostics) status += " — " + diagnostic.message;
    if (request.space.gamma == TransferFunction::DaVinciIntermediate)
      status += " — warning: ICC cannot fully represent DaVinci Intermediate scene values above 1.0; external apps may clip highlights";
    app.setStatus(status);
    return true;
  } catch (...) {
    publishExportException(app, std::current_exception());
    return false;
  }
}
} // namespace

bool runExportJob(App &app, const std::string &outPath) {
  if (app.full.px.empty() || !app.hasProcessingOutput() || outPath.empty()) return false;
  auto ownership = app.renderer.acquireExport();
  if (!ownership) return false;
  try {
    if (refuseActiveSourceOverwrite(app, outPath)) return false;
    app.setStatus("Exporting full resolution...");
    return executeExportJob(app, captureExportRequest(app, outPath));
  } catch (...) {
    publishExportException(app, std::current_exception());
    return false;
  }
}

bool startExport(App &app, const std::string &outPath) {
  if (app.full.px.empty() || !app.hasProcessingOutput() || outPath.empty()) return false;
  auto ownership = app.renderer.acquireExport();
  if (!ownership) return false;
  try {
    if (refuseActiveSourceOverwrite(app, outPath)) return false;
    app.setStatus("Exporting full resolution...");
    auto request = captureExportRequest(app, outPath);
    ownership.start([&app, request = std::move(request)]() noexcept { executeExportJob(app, request); });
    return true;
  } catch (...) {
    publishExportException(app, std::current_exception());
    return false;
  }
}
