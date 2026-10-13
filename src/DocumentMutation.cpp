#include "DocumentMutation.h"

#include "color/TransferFunction.h"

using document_detail::DocumentMutation;

DocumentMutation::DocumentMutation(App &app)
    : app_(app), interrupted_(app.renderer.beginMutation()) {}

DocumentMutation::~DocumentMutation() {
  app_.renderer.completeMutation(changed_, interrupted_);
}

void DocumentMutation::assertControlThread(const App &app) {
  app.renderer.assertControlThread();
}

std::shared_ptr<const Image> document_detail::makePreviewSource(const Image &full, ColorEncoding inputEncoding,
                                                                int previewRes) {
  const int maxEdge = kPreviewRes[std::clamp(previewRes, 0, kPreviewResCount - 1)].maxEdge;
  auto preview = std::make_shared<Image>();

  if (inputEncoding.gamma != TransferFunction::Linear && maxEdge > 0) {
    // Resample encoded working buffers in linear light, then restore the
    // selected encoding. Raster inputs are normally already linearised by the
    // loader; non-linear RAW working encodings still take this path.
    Image linear = full;
    for (size_t i = 0; i + 3 < linear.px.size(); i += 4) {
      linear.px[i + 0] = (float)decodeTransfer(linear.px[i + 0], inputEncoding.gamma);
      linear.px[i + 1] = (float)decodeTransfer(linear.px[i + 1], inputEncoding.gamma);
      linear.px[i + 2] = (float)decodeTransfer(linear.px[i + 2], inputEncoding.gamma);
    }
    makePreview(linear, maxEdge, *preview);
    for (size_t i = 0; i + 3 < preview->px.size(); i += 4) {
      preview->px[i + 0] = (float)encodeTransfer(preview->px[i + 0], inputEncoding.gamma);
      preview->px[i + 1] = (float)encodeTransfer(preview->px[i + 1], inputEncoding.gamma);
      preview->px[i + 2] = (float)encodeTransfer(preview->px[i + 2], inputEncoding.gamma);
    }
  } else {
    makePreview(full, maxEdge, *preview);
  }
  return preview;
}

void document_detail::DocumentMutation::rebuildPreview() {
  if (app_.full.px.empty()) return;
  ColorEncoding inputEncoding;
  {
    std::lock_guard<std::mutex> lock(app_.colorMutex);
    inputEncoding = app_.inputEncoding;
  }
  app_.preview = makePreviewSource(app_.full, inputEncoding, app_.previewRes);
  changed();
}

void rebuildPreview(App &app) {
  if (app.full.px.empty()) return;
  document_detail::DocumentMutation mutation(app);
  mutation.rebuildPreview();
}
