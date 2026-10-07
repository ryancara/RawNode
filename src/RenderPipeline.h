#pragma once

#include "AppState.h"
#include "processors/Processor.h"

// Evaluator only: the caller owns processor execution/lifetime. An empty token
// evaluates without cancellation (export and direct, worker-free self-tests).
ProcessorResult renderChain(App &app, const Image &src, Image &out,
                            const RenderCancellation &cancellation = {});

// UI thread with a current GL context. The runtime publishes CPU buffers below;
// pumpDisplayUpload consumes them for the viewer.
void uploadTexture(App &app, const Image &img);
void pumpDisplayUpload(App &app);

namespace display_detail {
// Runtime publication boundary. Conversion and the display-buffer lock stay in
// the presentation path; callers must hold runtime execution/document ownership.
void showSourcePreview(App &app);
void recolorDisplay(App &app);
void publishPreview(App &app, Image result, int generation);
}
