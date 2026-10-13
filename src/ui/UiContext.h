#pragma once

#include "AppState.h"

void drawLeftPanel(App &app);
void drawRightPanel(App &app);
void drawPreviewPanel(App &app);
void drawFilmstripPanel(App &app);
void doExport(App &app);  // Choose a destination, then start the export job.
void DrawUiFrame(App &app);

// Transfer actions capture before writing. An empty preset destination opens
// the save dialog; callers with a destination use the same capture/write path.
bool copySelectedNode(App &app);
bool copyGrade(App &app);
bool pasteFromClipboard(App &app);
bool saveNodePreset(App &app, std::string path = {});
bool saveGradePreset(App &app, std::string path = {});
