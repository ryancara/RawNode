#include "RenderPipeline.h"
#include "perf.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

static void uploadTextureRGBA(App &app, const unsigned char *rgba, int w, int h);

namespace {
float boundedCoverage(float coverage) {
  return std::isfinite(coverage) ? std::clamp(coverage, 0.0f, 1.0f) : 0.0f;
}

// Normalized lower-left edge coordinates -> bottom-up pixel-centre lattice.
// Outside the domain is empty; inside, missing bilinear taps are also zero.
// Never extend boundary coverage by clamping sample coordinates to the edge.
float sampleCoverage(const Mask &mask, double u, double v) {
  if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1) return 0;
  const double x = u * mask.w - .5, y = v * mask.h - .5;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const double fx = x - x0, fy = y - y0;
  const auto tap = [&](int i, int j) {
    return i < 0 || i >= mask.w || j < 0 || j >= mask.h ? 0.0f
        : boundedCoverage(mask.px[size_t(j) * mask.w + i]);
  };
  return boundedCoverage(float((1 - fy) * ((1 - fx) * tap(x0, y0) + fx * tap(x0 + 1, y0)) +
                                    fy * ((1 - fx) * tap(x0, y0 + 1) + fx * tap(x0 + 1, y0 + 1))));
}
}

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
  if (!app.displayRGBA.empty() && app.display.w > 0 && app.display.h > 0) {
    uploadTextureRGBA(app, app.displayRGBA.data(), app.display.w, app.display.h);
    app.textureGeneration = app.processed.generation;
    app.textureRequest = app.processed.request;
  } else {
    if (app.tex) glDeleteTextures(1, &app.tex);
    app.tex = 0; app.texW = app.texH = 0;
    app.textureGeneration = -1;
    app.textureRequest.reset();
  }
  app.displayDirty = false;
}

display_detail::Presentation display_detail::preparePresentation(
    const App &app, const EvaluationResult &result, const std::optional<EvaluationResult> &overlayMask) {
  Presentation presentation;
  auto &rgba = presentation.rgba;
  auto &w = presentation.w;
  auto &h = presentation.h;
  if (result.state != EvaluationState::Available) return presentation;
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
  if (overlayMask && overlayMask->state == EvaluationState::Available &&
      std::holds_alternative<ImageValue>(result.value)) {
    const auto *mask = std::get_if<MaskValue>(&overlayMask->value);
    const auto placement = resolveMaskPlacement(result, overlayMask->requested);
    const auto imageToMask = placement.maskToImage.inverse();
    if (mask && *mask && (*mask)->w > 0 && (*mask)->h > 0 &&
        (*mask)->px.size() / size_t((*mask)->h) == size_t((*mask)->w) &&
        (*mask)->px.size() % size_t((*mask)->h) == 0 &&
        imageToMask.kind() != CoordinateMapping::Kind::Unknown) {
      const bool direct = placement.state == MaskPlacementState::Identity && (*mask)->w == w && (*mask)->h == h;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          float coverage = 0;
          if (direct) coverage = boundedCoverage((*mask)->px[static_cast<size_t>(h - 1 - y) * w + x]);
          else if (const auto point = imageToMask.map((x + .5) / w, (h - y - .5) / h))
            coverage = sampleCoverage(**mask, point->first, point->second);
          const float amount = 0.4f * coverage;
          const size_t i = (static_cast<size_t>(y) * w + x) * 4;
          for (size_t channel = 0; channel < 3; ++channel) {
            const float target = channel == 0 ? 255.0f : 0.0f;
            rgba[i + channel] = static_cast<unsigned char>(rgba[i + channel] +
                (target - rgba[i + channel]) * amount + 0.5f);
          }
        }
    }
  }
  return presentation;
}

void display_detail::clearPresentation(App &app) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  app.displayRGBA.clear();
  app.display = {};
  app.displayDirty = true;
}

void display_detail::commitDisplayRefresh(App &app, const App::PreviewPublication &snapshot, Presentation presentation) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  if (app.processed.generation != snapshot.generation || app.processed.request != snapshot.request) return;
  app.displayRGBA = std::move(presentation.rgba);
  app.display = {presentation.w, presentation.h};
  app.displayDirty = true;
}

void display_detail::publishPreview(App &app, EvaluationResult result, int generation, uint64_t demand,
                                   std::shared_ptr<const OutputRef> request, Presentation presentation,
                                   std::optional<EvaluationResult> overlayMask) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  app.processed = {generation, demand, std::move(request), std::move(result), std::move(overlayMask)};
  app.display = {presentation.w, presentation.h};
  app.displayRGBA = std::move(presentation.rgba);
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
