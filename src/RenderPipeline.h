#pragma once

#include "AppState.h"
#include "processors/Processor.h"

#include <functional>

void waitRenderIdle(App &app);
void stopRenderWorker(App &app);
void beginRenderMutation(App &app);
void endRenderMutation(App &app);
void beginFullResolutionRender(App &app);
void endFullResolutionRender(App &app);
// UI/control-thread policy: export startup and interactive edits share that
// thread, so an allowed edit cannot be overtaken by export acquisition.
bool parameterEditingAllowed(App &app);
void scheduleRender(App &app);
void scheduleDisplayRecolor(App &app);
void rebuildPreview(App &app);
void uploadTexture(App &app, const Image &img);
void pumpDisplayUpload(App &app);
ProcessorResult renderChain(App &app, const Image &src, Image &out, int gen);
void renderWorker(App *app);
// Test observer runs under renderMutex immediately before the idle wait releases
// it. Reacquiring renderMutex after observing this callback proves worker parking.
// The observer must not call rendering APIs or acquire renderMutex itself.
void renderWorkerForSelfTest(App *app, const std::function<void()> &onIdle);
