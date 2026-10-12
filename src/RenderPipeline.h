#pragma once

#include "AppState.h"
#include "processors/Processor.h"

// Compatibility for existing direct pixel tests; delegates to evaluateGraph.
// Production preview/export consume typed EvaluationResult directly.
ProcessorResult renderChain(App &app, const Image &src, Image &out,
                            const RenderCancellation &cancellation = {});

// UI thread with a current GL context. The runtime publishes CPU buffers below;
// pumpDisplayUpload consumes them for the viewer.
void pumpDisplayUpload(App &app);

namespace display_detail {
// Runtime publication boundary. Conversion and the display-buffer lock stay in
// the presentation path; callers must hold runtime execution/document ownership.
void recolorDisplay(App &app);
void publishPreview(App &app, EvaluationResult result, int generation,
                    std::shared_ptr<const OutputRef> request);
}
