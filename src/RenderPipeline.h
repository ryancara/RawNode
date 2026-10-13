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
// Prepare under execution/document ownership, outside the runtime mutex. The
// final commit is gated by the runtime, then takes only the display-buffer lock.
struct Presentation {
  std::vector<unsigned char> rgba;
  int w = 0, h = 0;
};
Presentation preparePresentation(const App &app, const EvaluationResult &result);
void commitDisplayRefresh(App &app, const App::PreviewPublication &snapshot, Presentation presentation);
void publishPreview(App &app, EvaluationResult result, int generation,
                    std::shared_ptr<const OutputRef> request, Presentation presentation);
}
