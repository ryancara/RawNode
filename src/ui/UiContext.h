#pragma once

#include "AppState.h"

void drawLeftPanel(App &app);
void drawRightPanel(App &app);
void drawPreviewPanel(App &app);
void drawFilmstripPanel(App &app);
void doExport(App &app);  // Choose a destination, then start the export job.
void DrawUiFrame(App &app);
