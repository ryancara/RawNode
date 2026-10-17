#include "ui/UiContext.h"

#include "persist/DocumentActions.h"
#include "persist/ProjectPersist.h"
#include "NodeGraph.h"
#include "DocumentMutation.h"
#include "Export.h"
#include "ui/Themes.h"
#include "ui/DockLayout.h"

#include "imgui.h"
#include "portable-file-dialogs.h"

#include <GLFW/glfw3.h>

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
  startExport(app, outPath);
}

bool copySelectedNode(App &app) {
  if (!requireEditableGraph(app) || !app.selectedNodeId) return false;
  document_detail::DocumentMutation mutation(app);
  try {
    std::vector<std::string> warnings;
    auto fragment = captureFragment(app, {*app.selectedNodeId}, warnings);
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
  const bool applied = grade ? applyGrade(app, *grade)
      : pasteFragment(app, std::get<PersistFragment>(transfer), app.selectedNodeId);
  if (applied) app.setStatus(grade ? "Pasted full grade" : "Pasted node/fragment");
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

static bool loadPreset(App &app, bool append) {
  if (!requireEditableGraph(app)) return false;
  const auto paths = pfd::open_file(append ? "Append Preset" : "Replace Grade from Preset", "",
                                   {"RawNode grade", "*.json"}).result();
  if (paths.empty()) return false;
  PersistGrade grade;
  std::string error;
  if (!loadPresetFile(paths.front(), grade, error)) { app.setStatus(error); return false; }
  const bool applied = append ? appendGrade(app, grade, app.selectedNodeId) : applyGrade(app, grade);
  if (applied) app.setStatus(append ? "Appended preset" : "Replaced grade from preset");
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

void DrawUiFrame(App &app) {
  if (!app.preferencesWarning.empty()) {
    bool visible = true;
    if (ImGui::Begin("Application preferences warning", &visible, ImGuiWindowFlags_AlwaysAutoResize))
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
    openFolder(app, dir, promptSaveFailure);
  }

  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
#ifdef __APPLE__
      if (ImGui::MenuItem("Open image", "⌘+O")) {
#else
      if (ImGui::MenuItem("Open image", "Ctrl+O")) {
#endif
        auto f = pfd::open_file("Open image", "", openImageDialogFilters());
        auto r = f.result();
        if (!r.empty()) openPath(app, r[0], promptSaveFailure);
      }
#ifdef __APPLE__
      if (ImGui::MenuItem("Open Folder", "⌘+Shift+O")) {
#else
      if (ImGui::MenuItem("Open Folder", "Ctrl+Shift+O")) {
#endif
        auto f = pfd::select_folder("Open folder");
        auto r = f.result();
        if (!r.empty()) app.pendingFolderDir = r;
      }
      if (ImGui::MenuItem("Save Grade", nullptr, false, !app.path.empty() && !app.isPhotoProtected()))
        saveCurrentInputSidecar(app);
#ifdef __APPLE__
      if (ImGui::MenuItem("Export", "⌘+E")) doExport(app);
#else
      if (ImGui::MenuItem("Export", "Ctrl+E")) doExport(app);
#endif
#ifdef __APPLE__
      if (ImGui::MenuItem("Quit", "⌘+Q")) glfwSetWindowShouldClose(app.window, 1);
#else
      if (ImGui::MenuItem("Quit", "Ctrl+Q")) glfwSetWindowShouldClose(app.window, 1);
#endif
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
#ifdef __APPLE__
      const char *copyShortcut = "⌘+C";
      const char *pasteShortcut = "⌘+V";
#else
      const char *copyShortcut = "Ctrl+C";
      const char *pasteShortcut = "Ctrl+V";
#endif
      const Node *selection = selectedNode(app);
      const bool editable = !app.isPhotoProtected();
      const bool canCopyNode = editable && selection && selection->operation == NodeOperation::ImageProcessor;
      if (ImGui::MenuItem("Copy Node", copyShortcut, false, canCopyNode))
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
          if (ImGui::MenuItem("Replace…")) loadPreset(app, false);
          if (ImGui::MenuItem("Append…")) loadPreset(app, true);
          ImGui::EndMenu();
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
#ifdef __APPLE__
      ImGui::MenuItem("Left panel", "⌘+[", &app.showLeft);
      ImGui::MenuItem("Right panel", "⌘+]", &app.showRight);
      ImGui::MenuItem("Filmstrip", "⌘+\\", &app.showFilmstrip);
#else
      ImGui::MenuItem("Left panel", "Ctrl+[", &app.showLeft);
      ImGui::MenuItem("Right panel", "Ctrl+]", &app.showRight);
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

  if (!ImGui::GetIO().WantTextInput) {
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

  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_LeftBracket) ||
      ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_LeftBracket))
    app.showLeft = !app.showLeft;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_RightBracket) ||
      ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_RightBracket))
    app.showRight = !app.showRight;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backslash) ||
      ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_Backslash))
    app.showFilmstrip = !app.showFilmstrip;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) {
    auto f = pfd::select_folder("Open folder");
    auto r = f.result();
    if (!r.empty()) app.pendingFolderDir = r;
  }

  DockLayout::BeginMainDockSpace(app);

  if (app.showLeft) {
    if (ImGui::Begin(DockLayout::kLeft, &app.showLeft)) drawLeftPanel(app);
    ImGui::End();
  }
  if (app.showRight) {
    if (ImGui::Begin(DockLayout::kParams, &app.showRight)) drawRightPanel(app);
    ImGui::End();
  }
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
}
