#include "ui/UiContext.h"
#include "ui/ParamWidgets.h"

#include "persist/DocumentActions.h"
#include "persist/ProjectPersist.h"
#include "NodeGraph.h"
#include "Export.h"
#include "ui/Themes.h"
#include "ui/DockLayout.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "portable-file-dialogs.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

SaveFailureDecision promptSaveFailure(const std::string &error) {
  // Linux needs an installed desktop helper. Without one, keep the document;
  // the library's echo fallback cannot offer an explicit Discard choice.
  if (!pfd::settings::available()) return SaveFailureDecision::Cancel;
  // Spell out the three choices; dismissal or an unknown reply means Cancel.
  const auto answer = pfd::message("Could not save grade",
      error + "\n\nYes: Retry saving.\nNo: Discard the current grade and continue.\nCancel: Keep this photo open.",
      pfd::choice::yes_no_cancel, pfd::icon::warning).result();
  if (answer == pfd::button::yes) return SaveFailureDecision::Retry;
  if (answer == pfd::button::no) return SaveFailureDecision::Discard;
  return SaveFailureDecision::Cancel;
}

void doExport(App &app) {
  if (app.full.px.empty() || !app.hasProcessingOutput()) return;
  const char *exts[] = {".png", ".jpg"};
  const char *filters[] = {"PNG (8-bit)", "*.png", "JPEG", "*.jpg *.jpeg"};
  std::string def = fs::path(app.path).stem().string() + exts[app.exportFormat];
  auto sel = pfd::save_file("Export", def, {filters[app.exportFormat * 2], filters[app.exportFormat * 2 + 1]});
  std::string outPath = sel.result();
  if (outPath.empty()) return;
  if (fs::path(outPath).extension().empty()) outPath += exts[app.exportFormat];
  finishParameterGesture(app, true);
  startExport(app, outPath);
}

void finishUiEdits(App &app) {
  // Parameter completion may already have attempted a full capture.
  // Ending a simultaneous canvas interaction must not repeat that attempt.
  if (finishParameterGesture(app, true)) app.graphCanvas.positionEditPending = false;
  app.graphCanvas.cancelInteraction();
  ui::finishGraphDrag(app);
}

bool canCopySelectedNode(const App &app) {
  const Node *node = selectedNode(app);
  return !app.isPhotoProtected() && node && isProcessorOperation(node->operation);
}

bool copySelectedNode(App &app) {
  if (!requireEditableGraph(app) || !canCopySelectedNode(app)) return false;
  PersistFragment fragment;
  std::vector<std::string> warnings;
  if (!captureNodeFragment(app, {*app.selectedNodeId()}, fragment, warnings)) return false;
  try {
    std::string json, error;
    if (!encodeTransferPayload(fragment, json, error)) throw std::runtime_error(error);
    ImGui::SetClipboardText(json.c_str());
    std::string status = "Copied node";
    for (const auto &warning : warnings) status += "; " + warning;
    app.setStatus(status);
    return true;
  } catch (const std::exception &failure) { app.setStatus(failure.what()); return false; }
}

bool copyGrade(App &app) {
  PersistGrade grade;
  if (!captureGrade(app, grade)) return false;
  std::string json, error;
  if (!encodeTransferPayload(grade, json, error)) { app.setStatus(error); return false; }
  ImGui::SetClipboardText(json.c_str());
  app.setStatus("Copied full grade");
  return true;
}

bool pasteFromClipboard(App &app) {
  if (!requireEditableGraph(app)) return false;
  const char *clipboard = ImGui::GetClipboardText();
  PersistTransfer transfer;
  std::string error;
  if (!clipboard || !decodeTransferPayload(clipboard, transfer, error)) {
    app.setStatus("Clipboard does not contain supported RawNode data: " + error); return false;
  }
  const auto *grade = std::get_if<PersistGrade>(&transfer);
  finishUiEdits(app);
  const bool applied = grade ? applyGrade(app, *grade)
      : pasteFragment(app, std::get<PersistFragment>(transfer), selectedNode(app) ? app.selectedNodeId() : std::nullopt);
  if (applied) {
    if (grade) app.parameterGesture.reset();
    app.setStatus(grade ? "Pasted full grade" : "Pasted node/fragment");
  }
  return applied;
}

bool savePresetDialog(App &app, std::string path) {
  if (!requireEditableGraph(app)) return false;
  if (path.empty()) {
    // Confirmation is performed after extension normalization, so it always
    // names the exact file updated by the validated atomic writer.
    path = pfd::save_file("Save Preset", "Grade.rawnode.json", {"RawNode grade", "*.rawnode.json"},
                          pfd::opt::force_overwrite).result();
  }
  if (path.empty()) return false;
  if (fs::path(path).extension().empty()) path += ".rawnode.json";
  return savePreset(app, path, [](const std::string &destination) {
    if (!pfd::settings::available()) return false;
    return pfd::message("Replace preset?", "Replace the grade in " + destination + "?",
                         pfd::choice::yes_no, pfd::icon::warning).result() == pfd::button::yes;
  });
}

bool loadPresetDialog(App &app, bool append, std::string path) {
  if (!requireEditableGraph(app)) return false;
  if (path.empty()) {
    const auto paths = pfd::open_file(append ? "Append Preset" : "Replace Grade from Preset", "",
                                     {"RawNode grade", "*.json"}).result();
    if (paths.empty()) return false;
    path = paths.front();
  }
  PersistGrade grade;
  std::string error;
  if (!loadPresetFile(path, grade, error)) { app.setStatus(error); return false; }
  finishUiEdits(app);
  const bool applied = append ? appendGrade(app, grade, selectedNode(app) ? app.selectedNodeId() : std::nullopt) : applyGrade(app, grade);
  if (applied) {
    if (!append) app.parameterGesture.reset();
    app.setStatus(append ? "Appended preset" : "Replaced grade from preset");
  }
  return applied;
}

static void openUrl(const std::string &url) {
#if defined(_WIN32)
  std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
  std::string cmd = "open \"" + url + "\"";
#else
  std::string cmd = "xdg-open \"" + url + "\"";
#endif
  std::system(cmd.c_str());
}

static void openImageDialog(App &app) {
  const auto paths = pfd::open_file("Open image", "", openImageDialogFilters()).result();
  if (!paths.empty()) {
    finishUiEdits(app);
    if (openPath(app, paths.front(), promptSaveFailure)) app.parameterGesture.reset();
  }
}

static void openFolderDialog(App &app) {
  const auto path = pfd::select_folder("Open folder").result();
  if (!path.empty()) app.pendingFolderDir = path;
}

static bool canExport(const App &app) {
  // Parameter eligibility also excludes an active export and protected photos.
  return !app.full.px.empty() && app.hasProcessingOutput() && app.renderer.canEditParameters();
}

static void drawFileToolbar(App &app) {
  auto *viewport = ImGui::GetMainViewport();
  const float width = std::max(1.0f, viewport->Size.x - 2 * ImGui::GetStyle().WindowPadding.x);
  float height = ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().WindowPadding.y;
  if (!app.persistenceWarning.empty())
    height += ImGui::CalcTextSize(app.persistenceWarning.c_str(), nullptr, false, width).y +
              ImGui::GetStyle().ItemSpacing.y;
  const auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::BeginViewportSideBar("##FileToolbar", viewport, ImGuiDir_Up, height, flags)) {
    if (ImGui::Button("Open Image")) openImageDialog(app);
    ImGui::SameLine();
    if (ImGui::Button("Open Folder")) openFolderDialog(app);
    ImGui::SameLine();
    ImGui::BeginDisabled(!canExport(app));
    if (ImGui::Button("Export")) doExport(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto status = app.getStatus();
    ImGui::TextUnformatted(status.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status.c_str());
    if (!app.persistenceWarning.empty()) ImGui::TextWrapped("%s", app.persistenceWarning.c_str());
  }
  ImGui::End();
}

static bool historyIdle(const App &app) {
  return !app.path.empty() && app.renderer.canCaptureSnapshot() &&
         !app.parameterGesture && !app.graphCanvas.dragging && !app.graphCanvas.positionEditPending;
}

bool canUndoGrade(const App &app) { return historyIdle(app) && app.history.canUndo(); }
bool canRedoGrade(const App &app) { return historyIdle(app) && app.history.canRedo(); }

void DrawUiFrame(App &app) {
  if (!app.preferencesWarning.empty()) {
    bool visible = true;
    // Set the width every frame: old ini files may contain a collapsed width.
    const float width = std::max(1.0f, std::min(560.0f, ImGui::GetMainViewport()->WorkSize.x - 32.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    if (ImGui::Begin("Application preferences warning", &visible,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
      ImGui::TextWrapped("%s", app.preferencesWarning.c_str());
    ImGui::End();
    if (!visible) app.preferencesWarning.clear();
  }
  if (app.themeApplyPending) {
    applyTheme(app.themeIndex);
    app.themeApplyPending = false;
  }
  if (!app.pendingFolderDir.empty()) {
    const std::string dir = std::move(app.pendingFolderDir);
    app.pendingFolderDir.clear();
    finishUiEdits(app);
    openFolder(app, dir, promptSaveFailure);
  }

  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
#ifdef __APPLE__
      if (ImGui::MenuItem("Open image", "⌘+O")) {
#else
      if (ImGui::MenuItem("Open image", "Ctrl+O")) {
#endif
        openImageDialog(app);
      }
#ifdef __APPLE__
      if (ImGui::MenuItem("Open Folder", "⌘+Shift+O")) {
#else
      if (ImGui::MenuItem("Open Folder", "Ctrl+Shift+O")) {
#endif
        openFolderDialog(app);
      }
      if (ImGui::MenuItem("Save Grade", nullptr, false, !app.path.empty() && !app.isPhotoProtected()))
        saveCurrentInputSidecar(app);
#ifdef __APPLE__
      if (ImGui::MenuItem("Export", "⌘+E", false, canExport(app))) doExport(app);
#else
      if (ImGui::MenuItem("Export", "Ctrl+E", false, canExport(app))) doExport(app);
#endif
#ifdef __APPLE__
      if (ImGui::MenuItem("Quit", "⌘+Q")) glfwSetWindowShouldClose(app.window, 1);
#else
      if (ImGui::MenuItem("Quit", "Ctrl+Q")) glfwSetWindowShouldClose(app.window, 1);
#endif
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
      if (ImGui::MenuItem("Undo", mac ? "⌘+Z" : "Ctrl+Z", false, canUndoGrade(app))) app.history.undo(app);
      if (ImGui::MenuItem("Redo", mac ? "⌘+Shift+Z" : "Ctrl+Shift+Z / Ctrl+Y", false, canRedoGrade(app))) app.history.redo(app);
      ImGui::Separator();
#ifdef __APPLE__
      const char *copyShortcut = "⌘+C";
      const char *pasteShortcut = "⌘+V";
#else
      const char *copyShortcut = "Ctrl+C";
      const char *pasteShortcut = "Ctrl+V";
#endif
      const bool editable = !app.isPhotoProtected();
      if (ImGui::MenuItem("Copy Node", copyShortcut, false, canCopySelectedNode(app)))
        copySelectedNode(app);
#ifdef __APPLE__
      const char *copyGradeShortcut = "⌘+Shift+C";
#else
      const char *copyGradeShortcut = "Ctrl+Shift+C";
#endif
      if (ImGui::MenuItem("Copy Full Grade", copyGradeShortcut, false, editable))
        copyGrade(app);
      if (ImGui::MenuItem("Paste", pasteShortcut, false, editable))
        pasteFromClipboard(app);
      ImGui::Separator();
      if (ImGui::BeginMenu("Presets")) {
        if (ImGui::MenuItem("Save Preset…", nullptr, false, editable))
          savePresetDialog(app);
        if (ImGui::BeginMenu("Load Preset", editable)) {
          if (ImGui::MenuItem("Replace…")) loadPresetDialog(app, false);
          if (ImGui::MenuItem("Append…")) loadPresetDialog(app, true);
          ImGui::EndMenu();
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Nodes", nullptr, &app.showNodes);
#ifdef __APPLE__
      ImGui::MenuItem("Inspector", "⌘+]", &app.showRight);
      ImGui::MenuItem("Filmstrip", "⌘+\\", &app.showFilmstrip);
#else
      ImGui::MenuItem("Inspector", "Ctrl+]", &app.showRight);
      ImGui::MenuItem("Filmstrip", "Ctrl+\\", &app.showFilmstrip);
#endif
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Theme")) {
      for (int i = 0; i < themeCount(); ++i) {
        if (ImGui::MenuItem(themeName(i), nullptr, app.themeIndex == i)) {
          app.themeIndex = i;
          applyTheme(i);
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
      if (ImGui::MenuItem("About")) {
        app.showAbout = true;
      }
      if (ImGui::MenuItem("Donate")) {
        app.showDonate = true;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }

  if (!ImGui::GetIO().WantTextInput && !ImGui::GetInputTextState(ImGui::GetActiveID())) {
    const bool redo = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_None) ||
        (!ImGui::GetIO().ConfigMacOSXBehaviors && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_None));
    if (redo && canRedoGrade(app)) app.history.redo(app);
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_None) && canUndoGrade(app)) app.history.undo(app);
    // With ConfigMacOSXBehaviors enabled, Dear ImGui maps ImGuiMod_Ctrl to
    // Command on macOS and to Control on Windows/Linux.
    const ImGuiKeyChord copyNodeChord = ImGuiMod_Ctrl | ImGuiKey_C;
    const ImGuiKeyChord pasteChord = ImGuiMod_Ctrl | ImGuiKey_V;
    const ImGuiKeyChord copyGradeChord = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C;
    if (ImGui::IsKeyChordPressed(copyGradeChord))
      copyGrade(app);
    else if (ImGui::IsKeyChordPressed(copyNodeChord))
      copySelectedNode(app);
    if (ImGui::IsKeyChordPressed(pasteChord))
      pasteFromClipboard(app);
  }

  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_RightBracket) ||
      ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_RightBracket))
    app.showRight = !app.showRight;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backslash) ||
      ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_Backslash))
    app.showFilmstrip = !app.showFilmstrip;
  if (!ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) openFolderDialog(app);
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) openImageDialog(app);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E) && canExport(app)) doExport(app);
  }

  drawFileToolbar(app);
  DockLayout::BeginMainDockSpace(app);

  if (app.showRight) {
    ImGui::SetNextWindowSize(ImVec2(420, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(DockLayout::kInspector, &app.showRight)) drawInspector(app);
    ImGui::End();
  }
  if (app.showNodes) {
    ImGui::SetNextWindowSize(ImVec2(720, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(DockLayout::kNodes, &app.showNodes)) drawNodesPanel(app);
    else app.graphCanvas.cancelInteraction();
    ImGui::End();
  } else app.graphCanvas.cancelInteraction();
  {
    ImGuiWindowFlags previewFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin(DockLayout::kPreview, nullptr, previewFlags)) drawPreviewPanel(app);
    ImGui::End();
  }
  if (app.showFilmstrip) {
    if (ImGui::Begin(DockLayout::kFilmstrip, &app.showFilmstrip)) drawFilmstripPanel(app);
    ImGui::End();
  }

  if (app.showAbout) {
    ImGui::OpenPopup("About");
    app.showAbout = false;
  }
  if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("OfxRawHost");
    ImGui::Separator();
    ImGui::TextWrapped("A free and open-source OFX raw image host for color grading and plugin-based processing.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Repository:");
    if (ImGui::Button("github.com/aaronmurniadi/ofxrawhost")) {
      openUrl("https://github.com/aaronmurniadi/ofxrawhost");
    }
    ImGui::Spacing();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (app.showDonate) {
    ImGui::OpenPopup("Donate");
    app.showDonate = false;
  }
  if (ImGui::BeginPopupModal("Donate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("OfxRawHost is free and open-source. If you find it useful, consider supporting its development.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Buy me a coffee:");
    if (ImGui::Button("buymeacoffee.com/aaronmurniadi")) {
      openUrl("https://buymeacoffee.com/aaronmurniadi");
    }
    ImGui::Spacing();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  finishParameterGesture(app);
  ui::finishGraphDrag(app);
}
