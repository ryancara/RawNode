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

void waitRenderIdle(App &app) {
  ++gLatestGen;
  std::unique_lock<std::mutex> lock(app.renderMutex);
  app.renderPending = false;
}

void scheduleRender(App &app) {
  if (app.nodes.empty() || app.preview.px.empty()) {
    showSourcePreview(app);
    return;
  }
  for (auto &n : app.nodes)
    if (n.processor) n.processor->setRenderSize(app.preview.w, app.preview.h);
  ++gLatestGen;
  app.renderPending = true;
  app.renderCv.notify_one();
}

void rebuildPreview(App &app) {
  if (app.full.px.empty()) return;
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
  ++gLatestGen;
  std::lock_guard<std::mutex> lock(app.renderMutex);
  app.displayRecolorPending = true;
  app.renderPending = true;
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

void renderWorker(App *app) {
  while (!app->quit) {
    bool recolorOnly = false;
    {
      std::unique_lock<std::mutex> lock(app->renderMutex);
      app->renderCv.wait(lock, [&] { return app->quit || app->renderPending.load(); });
      if (app->quit) break;
      recolorOnly = app->displayRecolorPending;
      app->displayRecolorPending = false;
      app->renderPending = false;
    }
    if (recolorOnly) {
      Image img;
      {
        std::lock_guard<std::mutex> lock(app->displayMutex);
        if (app->display.px.empty()) continue;
        img = app->display;
      }
      std::vector<unsigned char> rgba;
      if (app->nodes.empty())
        sourceToDisplayRGBA8(*app, img, rgba);
      else {
        ColorEncoding outputEncoding;
        {
          std::lock_guard<std::mutex> colorLock(app->colorMutex);
          outputEncoding = app->outputEncoding;
        }
        toDisplayRGBA8(img, outputEncoding, rgba);
      }
      std::lock_guard<std::mutex> lock(app->displayMutex);
      app->displayRGBA = std::move(rgba);
      app->displayDirty = true;
      continue;
    }
    if (app->nodes.empty() || app->preview.px.empty()) continue;
    const int gen = ++gLatestGen;
    const int pw = app->preview.w;
    const int ph = app->preview.h;
    app->setStatus("Rendering...");
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
      app->setStatus(std::to_string(ow) + "×" + std::to_string(oh) + " preview");
    } else {
      app->setStatus("Render failed" + (result.message.empty() ? std::string() : ": " + result.message));
    }
  }
}
