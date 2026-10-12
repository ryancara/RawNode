#include "RenderPipeline.h"
#include "perf.h"

#include <GLFW/glfw3.h>
#include <algorithm>

static void uploadTextureRGBA(App &app, const unsigned char *rgba, int w, int h);

ProcessorResult renderChain(App &app, const Image &src, Image &out, const RenderCancellation &cancellation) {
  const EvaluationContext context{EvaluationPurpose::Inspection, src.w, src.h, src.w, src.h};
  const ImageValue source(&src, [](const Image *) {}); // Borrowed for this synchronous call only.
  const auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                    context, source, cancellation);
  if (result.state == EvaluationState::Available) {
    if (const auto *image = std::get_if<ImageValue>(&result.value)) {
      out = **image;
      return ProcessorResult::success();
    }
  }
  return ProcessorResult::failure(-1, result.diagnostics.empty() ? "Graph evaluation did not produce an Image"
                                                                : result.diagnostics.back().message);
}

void pumpDisplayUpload(App &app) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  if (!app.displayDirty) return;
  if (!app.displayRGBA.empty() && app.display.w > 0 && app.display.h > 0)
    uploadTextureRGBA(app, app.displayRGBA.data(), app.display.w, app.display.h);
  else {
    if (app.tex) glDeleteTextures(1, &app.tex);
    app.tex = 0; app.texW = app.texH = 0;
  }
  app.displayDirty = false;
}

static void presentation(const App &app, const EvaluationResult &result,
                         std::vector<unsigned char> &rgba, int &w, int &h) {
  w = h = 0;
  if (result.state != EvaluationState::Available) return;
  if (const auto *image = std::get_if<ImageValue>(&result.value)) {
    ColorEncoding encoding;
    {
      std::lock_guard<std::mutex> lock(app.colorMutex);
      // Source is explicitly input-interpreted; all other Image endpoints use
      // the document's manual output interpretation, not inferred metadata.
      encoding = result.requested == OutputRef{app.primarySource, ports::image} ? app.inputEncoding : app.outputEncoding;
    }
    w = (*image)->w; h = (*image)->h;
    toDisplayRGBA8(**image, encoding, rgba);
  } else if (const auto *mask = std::get_if<MaskValue>(&result.value)) {
    w = (*mask)->w; h = (*mask)->h;
    rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t sampleIndex = static_cast<size_t>(h - 1 - y) * w + x;
        const auto sample = static_cast<unsigned char>(std::clamp((*mask)->px[sampleIndex], 0.0f, 1.0f) * 255.0f + 0.5f);
        const size_t i = (static_cast<size_t>(y) * w + x) * 4;
        rgba[i] = rgba[i + 1] = rgba[i + 2] = sample; rgba[i + 3] = 255;
      }
  }
}

// Runtime holds execution ownership and its publication gate. Display conversion
// never takes the runtime lock itself, preserving the leaf-lock ordering.
void display_detail::recolorDisplay(App &app) {
  EvaluationResult result{{app.primarySource, ports::image}};
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    result = app.processed.result; // Shared immutable values; no raster copy.
  }
  std::vector<unsigned char> rgba;
  int w, h;
  presentation(app, result, rgba, w, h);
  std::lock_guard<std::mutex> lock(app.displayMutex);
  app.displayRGBA = std::move(rgba);
  app.display = {w, h};
  app.displayDirty = true;
}

void display_detail::publishPreview(App &app, EvaluationResult result, int generation,
                                   std::shared_ptr<const OutputRef> request) {
  std::vector<unsigned char> rgba;
  int w, h;
  presentation(app, result, rgba, w, h);
  std::lock_guard<std::mutex> lock(app.displayMutex);
  app.processed = {generation, std::move(request), std::move(result)};
  app.display = {w, h};
  app.displayRGBA = std::move(rgba);
  app.displayDirty = true;
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
