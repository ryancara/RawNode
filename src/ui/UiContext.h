#pragma once

#include "AppState.h"
#include "persist/DocumentActions.h"
#include "persist/PhotoTrash.h"

void drawProcessorPicker(App &app);
void drawInspector(App &app);
void drawNodesPanel(App &app);
void drawPresetPanel(App &app);
// Headless interaction tests replace only the GL upload boundary.
void drawPreviewPanel(App &app, void (*upload)(App &) = nullptr);
SaveFailureDecision promptSaveFailure(const std::string &error);
bool promptPhotoTrash(const std::string &message);
void reportPhotoTrash(const PhotoTrashResult &result);
// Operation-local confirmation/Trash seams keep real native Trash out of self-tests.
void drawFilmstripPanel(App &app, const SaveFailurePrompt &prompt = promptSaveFailure,
                        const PhotoTrashConfirmation &confirm = promptPhotoTrash,
                        const TrashOperation &move = moveFileToTrash,
                        const std::function<void(const PhotoTrashResult &)> &report = reportPhotoTrash);
bool navigateFilmstrip(App &app, bool forward, const SaveFailurePrompt &prompt, std::string &error);
void DrawUiFrame(App &app);
bool canUndoGrade(const App &app);
bool canRedoGrade(const App &app);
// Finish outgoing Inspector/canvas edits before UI navigation or replacement.
void finishUiEdits(App &app);
namespace document_detail { struct ActionTestOptions; }
// Shared GLFW drop dispatch, with the existing operation-local file-fault seam.
bool openDroppedPhoto(App &app, const std::string &path, const SaveFailurePrompt &prompt,
                      const document_detail::ActionTestOptions &options);

// Transfer actions capture before writing. An empty preset destination opens
// the save dialog; callers with a destination use the same capture/write path.
// Shared menu/shortcut eligibility includes unavailable processor operations.
bool canCopySelectedNode(const App &app);
bool copySelectedNode(App &app);
bool copyGrade(App &app);
bool pasteFromClipboard(App &app);
bool savePresetDialog(App &app, std::string path = {});
// A supplied path skips only the native chooser, retaining the UI anchor policy.
bool loadPresetDialog(App &app, bool append, std::string path = {});

namespace ui {
inline const KeyboardBindings &effectiveKeyboardBindings(const App &app) {
  if (app.keyboardBindings) return *app.keyboardBindings;
  return defaultKeyboardBindings(ImGui::GetIO().ConfigMacOSXBehaviors);
}
bool executeCommands(App &app, const CommandSequence &commands, const SaveFailurePrompt &prompt = promptSaveFailure);
void drawShortcutSettings(App &app);
}
