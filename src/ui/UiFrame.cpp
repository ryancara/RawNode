#include "ui/UiContext.h"

#include "persist/DocumentActions.h"
#include "persist/ProjectPersist.h"
#include "NodeGraph.h"
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
  if (!requireEditableGraph(app)) return false;
  PersistNode node;
  if (!app.selectedNodeId) return false;
  if (!captureNode(app, *app.selectedNodeId, node)) {
    app.setStatus("Node copy is unavailable for this graph in the current file format");
    return false;
  }

  PersistChain transfer;
  transfer.selectedNodeId = node.id;
  transfer.nodes.push_back(std::move(node));
  const std::string payload = serializeTransferPayload("node", transfer);
  ImGui::SetClipboardText(payload.c_str());
  app.setStatus("Copied node");
  return true;
}

bool copyGrade(App &app) {
  if (!requireEditableGraph(app)) return false;
  if (!app.hasProcessingOutput()) return false;
  PersistChain transfer;
  try { transfer = captureChain(app); }
  catch (const std::runtime_error &error) { app.setStatus(error.what()); return false; }
  const bool colourProtected =
      (!app.path.empty() && app.sidecarWriteBlockedPath == app.path) || app.workspaceWriteBlocked;
  if (colourProtected) {
    const std::string payload = serializeTransferPayload("grade", transfer);
    ImGui::SetClipboardText(payload.c_str());
    app.setStatus("Copied full grade without colour settings (source metadata is read-only)");
    return true;
  }

  const PersistGradeColor color = captureGradeColor(app);
  const std::string payload = serializeTransferPayload("grade", transfer, &color);
  ImGui::SetClipboardText(payload.c_str());
  app.setStatus("Copied full grade");
  return true;
}


bool pasteFromClipboard(App &app) {
  if (!requireEditableGraph(app)) return false;
  const char *clipboard = ImGui::GetClipboardText();
  if (!clipboard || !*clipboard) {
    app.setStatus("Clipboard does not contain RawNode data");
    return false;
  }

  std::string kind;
  PersistChain transfer;
  PersistGradeColor color;
  if (!parseTransferPayload(clipboard, kind, transfer, &color)) {
    app.setStatus("Clipboard does not contain RawNode data");
    return false;
  }

  if (kind == "node") {
    if (transfer.nodes.size() != 1) {
      app.setStatus("Invalid RawNode node payload");
      return false;
    }
    if (!appendPersistedNode(app, transfer.nodes.front(), app.selectedNodeId)) {
      app.setStatus("Could not paste node: " + app.getStatus());
      return false;
    }
    if (Node *node = selectedNode(app); node && node->isMissingProcessor())
      app.setStatus("Pasted node (processor unavailable)");
    else
      app.setStatus("Pasted node");
    return true;
  }

  if (kind == "grade") {
    if (!applyGrade(app, transfer, color)) return false;
    int unavailable = 0;
    for (const Node &node : app.nodes)
      if (node.isMissingProcessor()) ++unavailable;
    if (unavailable > 0)
      app.setStatus("Pasted full grade (" + std::to_string(unavailable) + " processor(s) unavailable)");
    else
      app.setStatus("Pasted full grade");
    return true;
  }

  app.setStatus("Unsupported RawNode clipboard data");
  return false;
}

static std::string ensurePresetExtension(std::string path) {
  if (!path.empty() && std::filesystem::path(path).extension().empty())
    path += ".rawnodepreset";
  return path;
}

bool saveNodePreset(App &app, std::string path) {
  if (!requireEditableGraph(app)) return false;
  PersistNode node;
  if (!app.selectedNodeId) return false;
  if (!captureNode(app, *app.selectedNodeId, node)) {
    app.setStatus("Node preset capture is unavailable for this graph in the current file format");
    return false;
  }

  PersistChain preset;
  preset.selectedNodeId = node.id;
  preset.nodes.push_back(std::move(node));

  if (path.empty()) {
    auto dialog = pfd::save_file("Save Node Preset", "Node.rawnodepreset", {"RawNode preset", "*.rawnodepreset"});
    path = dialog.result();
  }
  path = ensurePresetExtension(std::move(path));
  if (path.empty()) return false;

  if (!savePresetFile(path, "node", preset)) {
    app.setStatus("Could not save node preset");
    return false;
  }
  app.setStatus("Saved node preset: " + std::filesystem::path(path).filename().string());
  return true;
}

bool saveGradePreset(App &app, std::string path) {
  if (!requireEditableGraph(app)) return false;
  if (!app.hasProcessingOutput()) return false;
  PersistChain preset;
  try { preset = captureChain(app); }
  catch (const std::runtime_error &error) { app.setStatus(error.what()); return false; }
  const bool colourProtected =
      (!app.path.empty() && app.sidecarWriteBlockedPath == app.path) || app.workspaceWriteBlocked;

  if (path.empty()) {
    auto dialog = pfd::save_file("Save Full Grade Preset", "Grade.rawnodepreset", {"RawNode preset", "*.rawnodepreset"});
    path = dialog.result();
  }
  path = ensurePresetExtension(std::move(path));
  if (path.empty()) return false;

  bool ok = false;
  if (colourProtected) {
    ok = savePresetFile(path, "grade", preset);
  } else {
    const PersistGradeColor color = captureGradeColor(app);
    ok = savePresetFile(path, "grade", preset, &color);
  }
  if (!ok) {
    app.setStatus("Could not save full-grade preset");
    return false;
  }
  if (colourProtected)
    app.setStatus("Saved full-grade preset without colour settings (source metadata is read-only)");
  else
    app.setStatus("Saved full-grade preset: " + std::filesystem::path(path).filename().string());
  return true;
}


static bool loadPreset(App &app) {
  if (!requireEditableGraph(app)) return false;
  auto dialog = pfd::open_file(
      "Load RawNode Preset", "",
      {"RawNode preset", "*.rawnodepreset"});
  const auto paths = dialog.result();
  if (paths.empty()) return false;

  std::string kind;
  PersistChain preset;
  PersistGradeColor color;
  if (!loadPresetFile(paths[0], kind, preset, &color)) {
    app.setStatus("Could not read RawNode preset");
    return false;
  }

  if (kind == "node") {
    if (preset.nodes.size() != 1) {
      app.setStatus("Invalid node preset");
      return false;
    }
    if (!appendPersistedNode(app, preset.nodes.front(), app.selectedNodeId)) {
      app.setStatus("Could not apply node preset: " + app.getStatus());
      return false;
    }
    if (Node *node = selectedNode(app); node && node->isMissingProcessor())
      app.setStatus("Loaded node preset (processor unavailable)");
    else
      app.setStatus("Loaded node preset");
    return true;
  }

  if (kind == "grade") {
    if (!applyGrade(app, preset, color)) return false;
    int unavailable = 0;
    for (const Node &node : app.nodes)
      if (node.isMissingProcessor()) ++unavailable;
    if (unavailable > 0)
      app.setStatus("Loaded full-grade preset (" + std::to_string(unavailable) + " processor(s) unavailable)");
    else
      app.setStatus("Loaded full-grade preset");
    return true;
  }

  app.setStatus("Unsupported RawNode preset");
  return false;
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
  if (app.themeApplyPending) {
    applyTheme(app.themeIndex);
    app.themeApplyPending = false;
  }
  if (!app.pendingWorkspaceDir.empty()) {
    const std::string dir = std::move(app.pendingWorkspaceDir);
    app.pendingWorkspaceDir.clear();
    openWorkspace(app, dir, promptSaveFailure);
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
        if (!r.empty()) app.pendingWorkspaceDir = r;
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
      if (ImGui::MenuItem("Copy Full Grade", copyGradeShortcut, false, editable && app.hasProcessingOutput()))
        copyGrade(app);
      if (ImGui::MenuItem("Paste", pasteShortcut, false, editable))
        pasteFromClipboard(app);
      ImGui::Separator();
      if (ImGui::BeginMenu("Presets")) {
        if (ImGui::MenuItem("Save Node Preset…", nullptr, false, canCopyNode))
          saveNodePreset(app);
        if (ImGui::MenuItem("Save Full Grade Preset…", nullptr, false, editable && app.hasProcessingOutput()))
          saveGradePreset(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Load Preset…", nullptr, false, editable))
          loadPreset(app);
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
    if (!r.empty()) app.pendingWorkspaceDir = r;
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
