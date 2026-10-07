#include "Export.h"

#include "AppState.h"
#include "RenderPipeline.h"

#include <exception>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

struct ExportRequest {
  Image source;
  std::string outPath;
  int previewWidth = 0, previewHeight = 0;
  ColorEncoding space;
  bool bypassedMissingProcessor = false;
  int jpegQuality = 92;
};

static ExportRequest captureExportRequest(App &app, const std::string &outPath);
static bool executeExportJob(App &app, const ExportRequest &request) noexcept;
static void publishExportException(App &app, std::exception_ptr error,
                                   std::exception_ptr restorationError = {}) noexcept;

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

namespace {

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
                                   std::exception_ptr restorationError) noexcept {
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
  request.jpegQuality = app.jpegQuality;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    request.space = app.outputEncoding;
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
