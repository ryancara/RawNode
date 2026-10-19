#pragma once

#include "AppState.h"
#include "persist/DocumentActions.h"

void drawProcessorPicker(App &app);
void drawInspector(App &app);
void drawNodesPanel(App &app);
void drawPreviewPanel(App &app);
void drawFilmstripPanel(App &app);
SaveFailureDecision promptSaveFailure(const std::string &error);
void doExport(App &app);  // Choose a destination, then start the export job.
void DrawUiFrame(App &app);

// Transfer actions capture before writing. An empty preset destination opens
// the save dialog; callers with a destination use the same capture/write path.
// Shared menu/shortcut eligibility includes unavailable ImageProcessors.
bool canCopySelectedNode(const App &app);
bool copySelectedNode(App &app);
bool copyGrade(App &app);
bool pasteFromClipboard(App &app);
bool savePresetDialog(App &app, std::string path = {});
// A supplied path skips only the native chooser, retaining the UI anchor policy.
bool loadPresetDialog(App &app, bool append, std::string path = {});
