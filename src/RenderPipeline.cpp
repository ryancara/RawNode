#include "RenderPipeline.h"
#include "perf.h"
#include "ofx/OfxHost.h"  // gLatestGen cancellation token; move to generic render state later.
#include "color/TransferFunction.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <vector>

// ImGui OpenGL3 backend loads GL symbols; do not include gl.h/gl3.h here.

static void sourceToDisplayRGBA8(const App &app, const Image &img, std::vector<unsigned char> &rgba) {
  ColorEncoding encoding;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    encoding = app.inputEncoding;
  }
  toDisplayRGBA8(img, encoding, rgba);
}

static void showSourcePreview(App &app) {
  if (app.preview.px.empty()) return;
  std::vector<unsigned char> rgba;
  sourceToDisplayRGBA8(app, app.preview, rgba);
  std::lock_guard<std::mutex> lock(app.displayMutex);
  app.display = app.preview;
  app.displayRGBA = std::move(rgba);
  app.displayDirty = true;
}

// Cancels queued processor renders and display recolours, then waits for owners.
// Callers needing a fresh preview afterward must explicitly schedule one.
void waitRenderIdle(App &app) {
  std::unique_lock<std::mutex> lock(app.renderMutex);
  ++gLatestGen;
  app.renderPending = false;
  app.renderQuietPending = false;
  app.displayRecolorPending = false;
  app.renderIdleCv.wait(lock, [&] { return !app.renderBusy && !app.exportBusy; });
  // Export completion may have restored a preview while we were waiting.
  // Do not let that request survive an explicit idle/cancellation barrier.
  app.renderPending = false;
  app.renderQuietPending = false;
}

void stopRenderWorker(App &app) {
  std::lock_guard<std::mutex> lock(app.renderMutex);
  app.quit = true;
  app.renderCv.notify_one();
}

void beginRenderMutation(App &app) {
  waitRenderIdle(app);
  std::lock_guard<std::mutex> lock(app.renderMutex);
  ++app.renderMutationDepth;
}

void endRenderMutation(App &app) {
  std::lock_guard<std::mutex> lock(app.renderMutex);
  if (app.renderMutationDepth > 0) --app.renderMutationDepth;
  if (app.renderMutationDepth == 0 && (app.renderPending || app.displayRecolorPending))
    app.renderCv.notify_one();
}

void beginFullResolutionRender(App &app) {
  waitRenderIdle(app);
  std::lock_guard<std::mutex> lock(app.renderMutex);
  app.exportBusy = true;
}

void endFullResolutionRender(App &app) {
  std::lock_guard<std::mutex> lock(app.renderMutex);
  app.exportBusy = false;
  // Export startup cancels queued/in-flight previews. Restore processor work
  // through the existing preview queue, atomically with releasing ownership.
  // The worker still honours the mutation and shutdown gates.
  // Preserve doExport's result unless an explicit edit already queued work.
  if (!app.renderPending) app.renderQuietPending = true;
  app.renderPending = true;
  app.renderIdleCv.notify_all();
  app.renderCv.notify_one();
}

void scheduleRender(App &app) {
  if (app.nodes.empty() || app.preview.px.empty()) {
    showSourcePreview(app);
    return;
  }
  std::lock_guard<std::mutex> lock(app.renderMutex);
  ++gLatestGen;
  app.renderQuietPending = false;
  app.renderPending = true;
  app.renderCv.notify_one();
}

void rebuildPreview(App &app) {
  if (app.full.px.empty()) return;
  waitRenderIdle(app);
  const int maxEdge = kPreviewRes[std::clamp(app.previewRes, 0, kPreviewResCount - 1)].maxEdge;
  ColorEncoding inputEncoding;
  {
    std::lock_guard<std::mutex> lock(app.colorMutex);
    inputEncoding = app.inputEncoding;
  }

  if (inputEncoding.gamma != TransferFunction::Linear && maxEdge > 0) {
    // Resample encoded working buffers in linear light, then restore the
    // selected encoding. Raster inputs are normally already linearised by the
    // loader; non-linear RAW working encodings still take this path.
    Image linear = app.full;
    for (size_t i = 0; i + 3 < linear.px.size(); i += 4) {
      linear.px[i + 0] = (float)decodeTransfer(linear.px[i + 0], inputEncoding.gamma);
      linear.px[i + 1] = (float)decodeTransfer(linear.px[i + 1], inputEncoding.gamma);
      linear.px[i + 2] = (float)decodeTransfer(linear.px[i + 2], inputEncoding.gamma);
    }
    makePreview(linear, maxEdge, app.preview);
    for (size_t i = 0; i + 3 < app.preview.px.size(); i += 4) {
      app.preview.px[i + 0] = (float)encodeTransfer(app.preview.px[i + 0], inputEncoding.gamma);
      app.preview.px[i + 1] = (float)encodeTransfer(app.preview.px[i + 1], inputEncoding.gamma);
      app.preview.px[i + 2] = (float)encodeTransfer(app.preview.px[i + 2], inputEncoding.gamma);
    }
  } else {
    makePreview(app.full, maxEdge, app.preview);
  }

  scheduleRender(app);
}

static void uploadTextureRGBA(App &app, const unsigned char *rgba, int w, int h) {
  PerfScope _ps("uploadTextureRGBA");
  if (!rgba || w <= 0 || h <= 0) return;
  if (!app.tex) glGenTextures(1, &app.tex);
  glBindTexture(GL_TEXTURE_2D, app.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (app.texW != w || app.texH != h) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    app.texW = w;
    app.texH = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  }
}

void uploadTexture(App &app, const Image &img) {
  std::vector<unsigned char> rgba;
  if (app.nodes.empty())
    sourceToDisplayRGBA8(app, img, rgba);
  else {
    ColorEncoding outputEncoding;
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      outputEncoding = app.outputEncoding;
    }
    toDisplayRGBA8(img, outputEncoding, rgba);
  }
  uploadTextureRGBA(app, rgba.data(), img.w, img.h);
}

void scheduleDisplayRecolor(App &app) {
  std::lock_guard<std::mutex> lock(app.renderMutex);
  // Recolour changes only the display transform; it must not cancel or queue
  // processor-chain work. A pending full render already uses the latest tag.
  app.displayRecolorPending = true;
  app.renderCv.notify_one();
}

void pumpDisplayUpload(App &app) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  if (app.displayDirty && !app.displayRGBA.empty() && app.display.w > 0 && app.display.h > 0) {
    uploadTextureRGBA(app, app.displayRGBA.data(), app.display.w, app.display.h);
    app.displayDirty = false;
  }
}

ProcessorResult renderChain(App &app, const Image &src, Image &out, int gen) {
  static thread_local Image cur, next;

  if (app.nodes.empty()) {
    out = src;
    return ProcessorResult::success();
  }

  cur.w = src.w;
  cur.h = src.h;
  cur.px = src.px;
  for (size_t i = 0; i < app.nodes.size(); ++i) {
    Node &n = app.nodes[i];
    if (!n.enabled || !n.processor) continue;

    const auto t0 = std::chrono::steady_clock::now();
    ProcessorResult result = n.processor->render(cur, next, gen);
    const auto t1 = std::chrono::steady_clock::now();
    perfLog(("node: " + n.processor->displayName()).c_str(),
            std::chrono::duration<double, std::milli>(t1 - t0).count());

    if (!result.ok) return result;
    if (gen != 0 && gen != gLatestGen)
      return ProcessorResult::failure(-1, "Render superseded");

    cur.swap(next);
  }
  out = std::move(cur);
  return ProcessorResult::success();
}

// Called only while the preview worker owns renderBusy.
static void recolorDisplay(App &app) {
  Image img;
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    if (app.display.px.empty()) return;
    img = app.display;
  }
  std::vector<unsigned char> rgba;
  if (app.nodes.empty())
    sourceToDisplayRGBA8(app, img, rgba);
  else {
    ColorEncoding outputEncoding;
    {
      std::lock_guard<std::mutex> colorLock(app.colorMutex);
      outputEncoding = app.outputEncoding;
    }
    toDisplayRGBA8(img, outputEncoding, rgba);
  }
  std::lock_guard<std::mutex> lock(app.displayMutex);
  app.displayRGBA = std::move(rgba);
  app.displayDirty = true;
}

static void runRenderWorker(App *app, const std::function<void()> &onIdle) {
  struct BusyGuard {
    App *app = nullptr;
    ~BusyGuard() {
      if (!app) return;
      {
        std::lock_guard<std::mutex> lock(app->renderMutex);
        app->renderBusy = false;
      }
      app->renderIdleCv.notify_all();
    }
  };

  while (!app->quit) {
    bool recolorOnly = false;
    bool quiet = false;
    int gen = 0;
    int pw = 0, ph = 0;
    {
      std::unique_lock<std::mutex> lock(app->renderMutex);
      app->renderCv.wait(lock, [&] {
        const bool ready = app->quit ||
                           ((app->renderPending.load() || app->displayRecolorPending) &&
                            !app->exportBusy && app->renderMutationDepth == 0);
        if (!ready && onIdle) onIdle();
        return ready;
      });
      if (app->quit) break;

      // Full renders take priority over queued recolours.
      recolorOnly = !app->renderPending.load() && app->displayRecolorPending;
      quiet = !recolorOnly && app->renderQuietPending;
      app->renderQuietPending = false;
      app->displayRecolorPending = false;
      app->renderPending = false;
      app->renderBusy = true;

      if (!recolorOnly) {
        // Assign the generation while holding renderMutex so waitRenderIdle()
        // cannot cancel a render and then have the worker overtake that cancel.
        gen = ++gLatestGen;
        pw = app->preview.w;
        ph = app->preview.h;
      }
    }
    BusyGuard busy{app};

    if (recolorOnly) {
      recolorDisplay(*app);
      continue;
    }

    if (app->nodes.empty() || app->preview.px.empty()) continue;

    // Size-dependent processor state is mutated only while this worker owns
    // renderBusy, so graph-changing UI actions cannot free or reconfigure it.
    for (auto &n : app->nodes)
      if (n.processor) n.processor->setRenderSize(pw, ph);

    if (!quiet) app->setStatus("Rendering...");
    Image out;
    const ProcessorResult result = renderChain(*app, app->preview, out, gen);
    if (gen != gLatestGen) continue;
    if (result.ok) {
      ColorEncoding outputEncoding;
      {
        std::lock_guard<std::mutex> colorLock(app->colorMutex);
        outputEncoding = app->outputEncoding;
      }
      std::vector<unsigned char> rgba;
      toDisplayRGBA8(out, outputEncoding, rgba);
      const int ow = out.w, oh = out.h;
      std::lock_guard<std::mutex> lock(app->displayMutex);
      app->display = std::move(out);
      app->displayRGBA = std::move(rgba);
      app->displayDirty = true;
      app->displayGen = gen;
      if (!quiet) app->setStatus(std::to_string(ow) + "×" + std::to_string(oh) + " preview");
    } else {
      // Keep the last good image in the current output encoding, even if a
      // recolour was consumed by superseded work or cancelled before this render.
      // Do this under BusyGuard, never by requeueing cancelled work.
      recolorDisplay(*app);
      app->setStatus("Render failed" + (result.message.empty() ? std::string() : ": " + result.message));
    }
  }
}

void renderWorker(App *app) {
  runRenderWorker(app, {});
}

void renderWorkerForSelfTest(App *app, const std::function<void()> &onIdle) {
  runRenderWorker(app, onIdle);
}
