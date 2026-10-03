#pragma once

#include "AppState.h"
#include "processors/Processor.h"

void waitRenderIdle(App &app);
void beginRenderMutation(App &app);
void endRenderMutation(App &app);
void beginFullResolutionRender(App &app);
void endFullResolutionRender(App &app);
void scheduleRender(App &app);
void scheduleDisplayRecolor(App &app);
void rebuildPreview(App &app);
void uploadTexture(App &app, const Image &img);
void pumpDisplayUpload(App &app);
ProcessorResult renderChain(App &app, const Image &src, Image &out, int gen);
void renderWorker(App *app);
