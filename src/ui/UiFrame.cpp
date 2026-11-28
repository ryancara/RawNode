#include "ui/UiContext.h"
#include "ui/CommandTargets.h"
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

static bool uiEditInProgress(const App &app) {
  return (app.parameterGesture && !app.parameterGesture->heldAdjustment) ||
         app.previewInteraction.drag != ui::PreviewDrag::None ||
         app.previewInteraction.editPending || app.graphCanvas.dragging || app.graphCanvas.positionEditPending;
}

static bool canExport(const App &app) {
  return !app.exportDialog.open && !app.renderer.exportInProgress() && !uiEditInProgress(app) &&
         (canExportCurrent(app) || !app.filmstripSelection.empty());
}

static void doExport(App &app) {
  ui::finishHeldPreviewAdjustment(app);
  app.exportDialog = {};
  app.exportDialog.open = true;
  app.exportDialog.scope = canExportCurrent(app) ? ExportScope::Current : ExportScope::Selected;
}

static void chooseExportDestination(App &app) {
  auto &dialog = app.exportDialog;
  if (dialog.scope == ExportScope::Selected) {
    auto folder = pfd::select_folder("Export to folder", dialog.destination).result();
    if (!folder.empty()) dialog.destination = std::move(folder);
    return;
  }
  const bool jpeg = app.exportFormat == 1;
  const char *extension = jpeg ? ".jpg" : ".png";
  const auto defaultName = fs::u8path(app.path).stem().u8string() + extension;
  const std::vector<std::string> filters = jpeg ? std::vector<std::string>{"JPEG", "*.jpg *.jpeg"} :
                                                std::vector<std::string>{"PNG (8-bit)", "*.png"};
  auto path = pfd::save_file("Export photograph", dialog.destination.empty() ? defaultName : dialog.destination,
                            filters, pfd::opt::force_overwrite).result();
  if (path.empty()) return;
  if (fs::u8path(path).extension().empty()) path += extension;
  dialog.destination = std::move(path);
  dialog.error.clear();
}

static void confirmExport(App &app) {
  auto &dialog = app.exportDialog;
  try {
    if (dialog.scope == ExportScope::Current) {
      auto extension = fs::u8path(dialog.destination).extension().u8string();
      for (char &ch : extension) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
      if (app.exportFormat == 1 ? extension != ".jpg" && extension != ".jpeg" : extension != ".png") {
        dialog.error = "Choose a filename that matches the export format.";
        return;
      }
    }
    auto session = std::make_unique<ExportSession>(app, dialog.scope, dialog.destination, dialog.existing);
    if (session->running()) dialog.session = std::move(session);
    else dialog.error = "Could not start export.";
  } catch (const std::exception &error) {
    dialog.error = error.what();
  }
}

static void drawExportDialog(App &app) {
  auto &dialog = app.exportDialog;
  if (!dialog.open) return;
  if (!ImGui::IsPopupOpen("Export photographs")) ImGui::OpenPopup("Export photographs");
  ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Export photographs", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  bool close = false;
  if (auto *session = dialog.session.get()) {
    const auto &targets = session->targets();
    const auto &results = session->results();
    if (session->running()) {
      const auto name = fs::u8path(targets[results.size()].source).filename().u8string();
      ImGui::TextWrapped("%s", name.c_str());
      ImGui::ProgressBar(float(results.size()) / float(targets.size()), ImVec2(-1, 0),
                         (std::to_string(results.size()) + " / " + std::to_string(targets.size())).c_str());
      ImGui::BeginDisabled(session->cancelling());
      if (ImGui::Button(session->cancelling() ? "Cancelling…" : "Cancel Export")) session->cancel();
      ImGui::EndDisabled();
      session->advance();
    } else {
      size_t exported = 0, skipped = 0, failed = 0;
      bool details = false;
      for (const auto &result : results) {
        exported += result.outcome == ExportOutcome::Exported;
        skipped += result.outcome == ExportOutcome::Skipped;
        failed += result.outcome == ExportOutcome::Failed;
        details |= result.outcome != ExportOutcome::Exported || !result.warnings.empty();
      }
      ImGui::Text("%zu exported, %zu skipped, %zu failed, %zu unprocessed",
                  exported, skipped, failed, targets.size() - results.size());
      if (details) {
        ImGui::BeginChild("Export details", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8));
        for (size_t i = 0; i < results.size(); ++i) {
          const auto &result = results[i];
          if (result.outcome == ExportOutcome::Exported && result.warnings.empty()) continue;
          auto name = fs::u8path(targets[i].source).filename().u8string();
          if (result.outcome == ExportOutcome::Exported) ImGui::TextWrapped("%s", name.c_str());
          else ImGui::TextWrapped("%s (%s) — %s", name.c_str(),
                                 result.outcome == ExportOutcome::Skipped ? "Skipped" : "Failed",
                                 result.message.empty() ? "Graph evaluation failed" : result.message.c_str());
          for (const auto &warning : result.warnings) ImGui::TextWrapped("%s", warning.c_str());
        }
        ImGui::EndChild();
      }
      close = ImGui::Button("Done");
    }
  } else {
    const auto selection = selectedPhotographsForExport(app);
    const bool currentAvailable = canExportCurrent(app);
    const auto setScope = [&](ExportScope scope) {
      if (dialog.scope == scope) return;
      dialog.scope = scope;
      dialog.destination.clear();
      dialog.error.clear();
    };
    ImGui::BeginDisabled(!currentAvailable);
    if (ImGui::RadioButton("Current Photograph", dialog.scope == ExportScope::Current)) setScope(ExportScope::Current);
    ImGui::EndDisabled();
    if (currentAvailable) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", fs::u8path(app.path).filename().u8string().c_str());
    }
    ImGui::BeginDisabled(selection.paths.empty());
    if (ImGui::RadioButton("Selected Photographs", dialog.scope == ExportScope::Selected)) setScope(ExportScope::Selected);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu selected, %zu hidden", selection.paths.size(), selection.hidden);

    ImGui::Separator();
    if (ImGui::Button(dialog.scope == ExportScope::Current ? "Choose filename…" : "Choose folder…"))
      chooseExportDestination(app);
    if (!dialog.destination.empty()) ImGui::TextWrapped("%s", dialog.destination.c_str());
    if (ImGui::Combo("Export format", &app.exportFormat, "PNG (8-bit)\0JPEG\0")) {
      if (dialog.scope == ExportScope::Current) dialog.destination.clear();
      dialog.error.clear();
    }
    if (app.exportFormat == 1) ImGui::SliderInt("JPEG quality", &app.jpegQuality, 1, 100);
    ImGui::TextUnformatted("Existing files");
    ImGui::SameLine();
    if (ImGui::RadioButton("Skip", dialog.existing == ExistingExport::Skip)) dialog.existing = ExistingExport::Skip;
    ImGui::SameLine();
    if (ImGui::RadioButton("Overwrite", dialog.existing == ExistingExport::Overwrite))
      dialog.existing = ExistingExport::Overwrite;
    if (!dialog.error.empty()) ImGui::TextWrapped("%s", dialog.error.c_str());

    const bool eligible = dialog.scope == ExportScope::Current ? currentAvailable : !selection.paths.empty();
    ImGui::BeginDisabled(dialog.destination.empty() || !eligible);
    if (ImGui::Button("Export")) confirmExport(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    close = ImGui::Button("Cancel");
  }
  if (close) {
    dialog = {};
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void finishUiEdits(App &app) {
  app.previewHeldAdjustment.reset();
  ui::finishPreviewInteraction(app, true);
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

static bool writeTransferClipboard(App &app, const std::string &json) {
  // ImGui's setter has no result. A chain may navigate and paste immediately.
  ImGui::SetClipboardText(json.c_str());
  const char *stored = ImGui::GetClipboardText();
  if (stored && json == stored) return true;
  app.setStatus("Could not copy RawNode data to the clipboard");
  return false;
}

bool copySelectedNode(App &app) {
  if (!requireEditableGraph(app) || !canCopySelectedNode(app)) return false;
  PersistFragment fragment;
  std::vector<std::string> warnings;
  if (!captureNodeFragment(app, {*app.selectedNodeId()}, fragment, warnings)) return false;
  try {
    std::string json, error;
    if (!encodeTransferPayload(fragment, json, error)) throw std::runtime_error(error);
    if (!writeTransferClipboard(app, json)) return false;
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
  if (!writeTransferClipboard(app, json)) return false;
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
    if (app.history.hasUnrecordedEdit()) return false;
    app.setStatus(grade ? "Pasted full grade" : "Pasted node/fragment");
  }
  return applied;
}

bool savePresetDialog(App &app, std::string path) {
  if (!requireEditableGraph(app)) return false;
  ui::finishHeldPreviewAdjustment(app);
  if (path.empty()) {
    // Confirmation is performed after extension normalization, so it always
    // names the exact file updated by the validated atomic writer.
    path = pfd::save_file("Save Preset", "Grade.rawnode.json", {"RawNode grade", "*.rawnode.json"},
                          pfd::opt::force_overwrite).result();
  }
  if (path.empty()) return false;
  if (!hasFilenameSuffix(path, ".rawnode.json")) path += ".rawnode.json";
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

static bool openImageDialog(App &app) {
  const auto paths = pfd::open_file("Open image", "", openImageDialogFilters()).result();
  if (paths.empty()) return false;
  finishUiEdits(app);
  if (!openPath(app, paths.front(), promptSaveFailure)) return false;
  app.parameterGesture.reset();
  return true;
}

static bool openFolderDialog(App &app) {
  const auto path = pfd::select_folder("Open folder").result();
  if (path.empty()) return false;
  app.pendingFolderDir = path;
  return true;
}

static bool drawDirectorySetting(const char *label, const char *pickerTitle, std::string &path,
                                 std::string &feedback) {
  bool changed = false;
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  if (ImGui::Button("Choose Folder...")) {
    if (!pfd::settings::available()) {
      feedback = "No system folder picker is available.";
    } else {
      auto selected = pfd::select_folder(pickerTitle, directoryPreferenceError(path).empty() ? path : "").result();
      if (!selected.empty()) {
        feedback = directoryPreferenceError(selected);
        if (feedback.empty()) {
          changed = path != selected;
          path = std::move(selected);
        } else feedback = "Could not use the selected folder. " + feedback;
      }
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(path.empty());
  if (ImGui::Button("Clear")) {
    changed = true;
    path.clear();
    feedback.clear();
  }
  ImGui::EndDisabled();
  if (path.empty()) ImGui::TextDisabled("No directory configured");
  else ImGui::TextWrapped("%s", path.c_str());
  if (!feedback.empty()) ImGui::TextWrapped("%s", feedback.c_str());
  ImGui::PopID();
  return changed;
}

static void appendPresetChoices(const DirectoryFolder &folder, const fs::path &root,
                                std::vector<std::string> &choices) {
  for (const auto &child : folder.folders) appendPresetChoices(child, root, choices);
  for (const auto &file : folder.files) {
    auto relative = file.lexically_relative(root).generic_u8string();
    if (validDefaultPresetPath(relative)) choices.push_back(std::move(relative));
  }
}

static void refreshDefaultPresetFeedback(App &app) {
  auto &feedback = app.settingsWindow.defaultPresetFeedback;
  feedback.clear();
  if (app.defaultPreset.empty()) return;
  PersistGrade grade;
  std::string error;
  if (!loadDefaultPreset(app, grade, error)) feedback = "Could not read default preset. " + error;
}

static void drawDefaultPresetSetting(App &app) {
  auto &window = app.settingsWindow;
  const char *preview = app.defaultPreset.empty() ? "None" : app.defaultPreset.c_str();
  if (ImGui::BeginCombo("Default preset", preview)) {
    if (ImGui::IsWindowAppearing()) {
      const auto listing = discoverDirectory(fs::u8path(app.presetDirectory), ".rawnode.json");
      window.defaultPresetChoices.clear();
      appendPresetChoices(listing.contents, listing.contents.path, window.defaultPresetChoices);
      window.defaultPresetScanError = listing.scanError;
      refreshDefaultPresetFeedback(app);
    }
    if (ImGui::Selectable("None", app.defaultPreset.empty())) {
      app.defaultPreset.clear();
      window.defaultPresetFeedback.clear();
    }
    for (size_t i = 0; i < window.defaultPresetChoices.size(); ++i) {
      const auto &choice = window.defaultPresetChoices[i];
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(choice.c_str(), app.defaultPreset == choice)) {
        app.defaultPreset = choice;
        refreshDefaultPresetFeedback(app);
      }
      ImGui::PopID();
    }
    if (window.defaultPresetScanError) ImGui::TextDisabled("Some preset locations could not be read.");
    ImGui::EndCombo();
  }
  if (!app.defaultPreset.empty()) ImGui::SetItemTooltip("%s", app.defaultPreset.c_str());
  ImGui::TextWrapped("Applies when opening a photograph without a saved grade.");
  if (!window.defaultPresetFeedback.empty()) ImGui::TextWrapped("%s", window.defaultPresetFeedback.c_str());
}

static void drawSettings(App &app, bool requested) {
  auto &window = app.settingsWindow;
  if (requested) {
    window.open = true;
    window.presetFeedback = directoryPreferenceError(app.presetDirectory);
    window.ctlScriptsFeedback = directoryPreferenceError(app.ctlScriptsDirectory);
    refreshDefaultPresetFeedback(app);
    ImGui::SetNextWindowFocus();
  }
  if (!window.open) return;
  ImGui::SetNextWindowSize(ImVec2(460, 400), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Settings", &window.open, ImGuiWindowFlags_NoDocking)) {
    ImGui::SeparatorText("Presets");
    if (drawDirectorySetting("Root preset directory", "Choose the root preset directory",
                             app.presetDirectory, window.presetFeedback)) {
      app.defaultPreset.clear();
      window.defaultPresetFeedback.clear();
    }
    drawDefaultPresetSetting(app);
    ImGui::Spacing();
    ImGui::SeparatorText("Processors");
    drawDirectorySetting("CTL scripts directory", "Choose the CTL scripts directory",
                         app.ctlScriptsDirectory, window.ctlScriptsFeedback);
    ImGui::Spacing();
    ui::drawShortcutSettings(app);
  }
  ImGui::End();
}

using Command = ui::ApplicationCommand;

static bool canInvokeCommand(const App &app, Command command) {
  switch (command) {
    case Command::OpenImage:
    case Command::OpenFolder: return !app.renderer.exportInProgress();
    case Command::Export: return canExport(app);
    case Command::Undo: return canUndoGrade(app);
    case Command::Redo: return canRedoGrade(app);
    case Command::CopyNode: return !app.renderer.exportInProgress() && canCopySelectedNode(app);
    case Command::CopyFullGrade:
    case Command::Paste: return !app.renderer.exportInProgress() && !app.isPhotoProtected();
    case Command::Inspector:
    case Command::Filmstrip: return true;
    case Command::SelectPosition:
    case Command::SelectNext:
    case Command::SelectPrevious:
    case Command::SelectProcessor:
    case Command::NextPhoto:
    case Command::PreviousPhoto: return !app.renderer.exportInProgress();
    case Command::MarkPick:
    case Command::MarkReject:
    case Command::MarkUnmarked: return !app.path.empty() && !app.isPhotoProtected() && !app.renderer.exportInProgress();
    case Command::SetValue:
    case Command::AdjustValue:
    case Command::Increase:
    case Command::Decrease:
    case Command::ResetParameter: return !app.isPhotoProtected() && app.renderer.canEditParameters();
    case Command::ToggleNode: return !app.isPhotoProtected() && app.renderer.canEditParameters() && selectedNode(app);
    case Command::AdjustWhileHeld:
    case Command::Count: break;
  }
  return false;
}

static bool invokeCommand(App &app, const ui::CommandStep &step, const SaveFailurePrompt &prompt,
                          std::string &error) {
  if (!canInvokeCommand(app, step.kind)) return false;
  switch (step.kind) {
    case Command::OpenImage: return openImageDialog(app);
    case Command::OpenFolder: return openFolderDialog(app);
    case Command::Export: doExport(app); return true; // Terminal request to show the export dialog.
    case Command::Undo: return app.history.undo(app);
    case Command::Redo: return app.history.redo(app);
    case Command::CopyNode: return copySelectedNode(app);
    case Command::CopyFullGrade: return copyGrade(app);
    case Command::Paste: return pasteFromClipboard(app);
    case Command::Inspector: app.showRight = !app.showRight; return true;
    case Command::Filmstrip: app.showFilmstrip = !app.showFilmstrip; return true;
    case Command::SelectPosition:
    case Command::SelectNext:
    case Command::SelectPrevious:
    case Command::SelectProcessor: return ui::selectCommandNode(app, step, error);
    case Command::SetValue:
      return ui::editCommandParameter(app, std::get<ui::ScalarCommandOperand>(step.operand), ui::ScalarEdit::Set, error);
    case Command::AdjustValue:
      return ui::editCommandParameter(app, std::get<ui::ScalarCommandOperand>(step.operand), ui::ScalarEdit::Adjust, error);
    case Command::Increase:
      return ui::editCommandParameter(app, std::get<ui::ScalarCommandOperand>(step.operand), ui::ScalarEdit::Increase, error);
    case Command::Decrease:
      return ui::editCommandParameter(app, std::get<ui::ScalarCommandOperand>(step.operand), ui::ScalarEdit::Decrease, error);
    case Command::ResetParameter:
      return ui::editCommandParameter(app, std::get<ui::ScalarCommandOperand>(step.operand), ui::ScalarEdit::Reset, error);
    case Command::ToggleNode: {
      const auto *node = selectedNode(app);
      return setNodeEnabled(app, node->id, !node->enabled);
    }
    case Command::NextPhoto: return navigateFilmstrip(app, true, prompt, error);
    case Command::PreviousPhoto: return navigateFilmstrip(app, false, prompt, error);
    case Command::MarkPick: return assignPhotoMark(app, app.path, PhotoMark::Pick);
    case Command::MarkReject: return assignPhotoMark(app, app.path, PhotoMark::Reject);
    case Command::MarkUnmarked: return assignPhotoMark(app, app.path, PhotoMark::Unmarked);
    case Command::AdjustWhileHeld:
    case Command::Count: break;
  }
  return false;
}

bool ui::executeCommands(App &app, const CommandSequence &commands, const SaveFailurePrompt &prompt) {
  app.shortcutFailure = sequenceError(commands);
  if (!app.shortcutFailure.empty()) return false;
  finishHeldPreviewAdjustment(app);
  for (size_t i = 0; i < commands.size(); ++i) {
    const auto &step = commands[i];
    std::string reason;
    if (invokeCommand(app, step, prompt, reason)) continue;
    // The original standalone shortcuts use the action's existing feedback.
    if (commands.size() == 1 && step.kind <= Command::Filmstrip) return false;
    if (reason.empty()) reason = "Did not complete";
    app.shortcutFailure = "Step " + std::to_string(i + 1) + " (" + commandInfo(step.kind).label + "): " + reason;
    return false;
  }
  return true;
}

static void drawCommandMenuItem(const App &app, std::optional<Command> &selectedCommand, Command command,
                                bool checked = false) {
  const auto shortcuts = ui::shortcutLabels(ui::effectiveKeyboardBindings(app), command,
                                            ImGui::GetIO().ConfigMacOSXBehaviors);
  if (ImGui::MenuItem(ui::commandInfo(command).label, shortcuts.empty() ? nullptr : shortcuts.c_str(),
                     checked, canInvokeCommand(app, command)))
    selectedCommand = command;
}

static bool keyboardShortcutBlocked(const App &app) {
  return app.exportDialog.open || uiEditInProgress(app) ||
         ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
}

static void dispatchKeyboardShortcut(App &app) {
  // Require an exact chord and a base key that ImGui has neither owned nor locked.
  // Panel shortcuts that poll without claiming keys are reserved at assignment.
  for (const auto &binding : ui::effectiveKeyboardBindings(app)) {
    if (binding.invokes(Command::AdjustWhileHeld)) continue;
    const auto key = static_cast<ImGuiKey>(binding.trigger & ~ImGuiMod_Mask_);
    if (ImGui::TestKeyOwner(key, ImGuiKeyOwner_Any) &&
        ImGui::IsKeyChordPressed(binding.trigger, ImGuiInputFlags_None, ImGuiKeyOwner_NoOwner)) {
      ui::executeCommands(app, binding.commands);
      return;
    }
  }
}

static void drawFileToolbar(App &app, std::optional<Command> &selectedCommand) {
  auto *viewport = ImGui::GetMainViewport();
  const float width = std::max(1.0f, viewport->Size.x - 2 * ImGui::GetStyle().WindowPadding.x);
  float height = ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().WindowPadding.y;
  if (!app.persistenceWarning.empty())
    height += ImGui::CalcTextSize(app.persistenceWarning.c_str(), nullptr, false, width).y +
              ImGui::GetStyle().ItemSpacing.y;
  if (!app.shortcutFailure.empty())
    height += ImGui::GetFrameHeightWithSpacing();
  const auto flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::BeginViewportSideBar("##FileToolbar", viewport, ImGuiDir_Up, height, flags)) {
    ImGui::BeginDisabled(app.renderer.exportInProgress());
    if (ImGui::Button("Open Image")) selectedCommand = Command::OpenImage;
    ImGui::SameLine();
    if (ImGui::Button("Open Folder")) selectedCommand = Command::OpenFolder;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canInvokeCommand(app, Command::Export));
    if (ImGui::Button("Export")) selectedCommand = Command::Export;
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto status = app.getStatus();
    ImGui::TextUnformatted(status.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status.c_str());
    if (!app.persistenceWarning.empty()) ImGui::TextWrapped("%s", app.persistenceWarning.c_str());
    if (!app.shortcutFailure.empty()) {
      if (ImGui::SmallButton("Dismiss")) app.shortcutFailure.clear();
      ImGui::SameLine();
      ImGui::TextUnformatted(app.shortcutFailure.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", app.shortcutFailure.c_str());
    }
  }
  ImGui::End();
}

static bool historyIdle(const App &app) {
  return !app.path.empty() && app.renderer.canCaptureSnapshot() && !uiEditInProgress(app);
}

bool canUndoGrade(const App &app) { return historyIdle(app) && app.history.canUndo(); }
bool canRedoGrade(const App &app) { return historyIdle(app) && app.history.canRedo(); }

void DrawUiFrame(App &app) {
  // Drawing can end an edit or close a popup before the frame's key is dispatched.
  const bool shortcutWasBlocked = keyboardShortcutBlocked(app);
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
  if (!app.exportDialog.open && !app.pendingFolderDir.empty()) {
    const std::string dir = std::move(app.pendingFolderDir);
    app.pendingFolderDir.clear();
    finishUiEdits(app);
    openFolder(app, dir, promptSaveFailure);
  }

  bool settingsRequested = false;
  std::optional<Command> selectedCommand;
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      drawCommandMenuItem(app, selectedCommand, Command::OpenImage);
      drawCommandMenuItem(app, selectedCommand, Command::OpenFolder);
      if (ImGui::MenuItem("Save Grade", nullptr, false, !app.path.empty() && !app.isPhotoProtected())) {
        ui::finishHeldPreviewAdjustment(app);
        saveCurrentInputSidecar(app);
      }
      drawCommandMenuItem(app, selectedCommand, Command::Export);
      ImGui::Separator();
      settingsRequested = ImGui::MenuItem("Settings...");
#ifdef __APPLE__
      // GLFW's native application menu owns Command+Q.
      const char *quitShortcut = "⌘+Q";
#else
      const char *quitShortcut = nullptr;
#endif
      if (ImGui::MenuItem("Quit", quitShortcut)) glfwSetWindowShouldClose(app.window, 1);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      drawCommandMenuItem(app, selectedCommand, Command::Undo);
      drawCommandMenuItem(app, selectedCommand, Command::Redo);
      ImGui::Separator();
      drawCommandMenuItem(app, selectedCommand, Command::CopyNode);
      drawCommandMenuItem(app, selectedCommand, Command::CopyFullGrade);
      drawCommandMenuItem(app, selectedCommand, Command::Paste);
      ImGui::Separator();
      const bool editable = !app.isPhotoProtected();
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
      drawCommandMenuItem(app, selectedCommand, Command::Inspector, app.showRight);
      drawCommandMenuItem(app, selectedCommand, Command::Filmstrip, app.showFilmstrip);
      ImGui::MenuItem("Preset Panel", nullptr, &app.showPresetPanel);
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
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }

  drawFileToolbar(app, selectedCommand);
  if (selectedCommand) {
    std::string reason;
    invokeCommand(app, *selectedCommand, promptSaveFailure, reason);
  }
  drawSettings(app, settingsRequested);
  drawExportDialog(app);
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
    else ui::finishHeldPreviewAdjustment(app);
    ImGui::End();
  }
  if (app.showFilmstrip) {
    if (ImGui::Begin(DockLayout::kFilmstrip, &app.showFilmstrip)) drawFilmstripPanel(app);
    ImGui::End();
  }

  drawPresetPanel(app);

  if (app.showAbout) {
    ImGui::OpenPopup("About RawNode");
    app.showAbout = false;
  }
  if (ImGui::BeginPopupModal("About RawNode", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("A free and open-source RAW photo editor.");
    ImGui::TextWrapped("Derived from OFX Raw Host by Aaron Murniadi.");
    ImGui::Spacing();
    if (ImGui::Button("github.com/ryancara/RawNode")) {
      openUrl("https://github.com/ryancara/RawNode");
    }
    ImGui::Spacing();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (!selectedCommand && !shortcutWasBlocked && !keyboardShortcutBlocked(app))
    dispatchKeyboardShortcut(app);

  if (!app.exportDialog.open) {
    finishParameterGesture(app);
    ui::finishPreviewInteraction(app);
    ui::finishGraphDrag(app);
  }
}
