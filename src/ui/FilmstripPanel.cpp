#include "ui/UiContext.h"
#include "ui/ParamWidgets.h"

#include "persist/DocumentActions.h"
#include "ui/Filmstrip.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

void drawFilmstripPanel(App &app, const SaveFailurePrompt &prompt) {
  static const struct {
    const char *label;
    const char *indicator;
    PhotoMark mark;
  } markChoices[] = {{"Pick", "P", PhotoMark::Pick}, {"Unmarked", "U", PhotoMark::Unmarked},
                     {"Reject", "R", PhotoMark::Reject}};
  const auto &style = ImGui::GetStyle();
  // Keep this toolbar compact even in themes with tall ordinary controls.
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 1));
  const float filterWidth = ImGui::CalcTextSize("Unmarked").x + ImGui::GetFrameHeight() * 1.5f;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x, 0));
  if (ImGui::BeginTable("##filmstripToolbar", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Mark", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Filter", ImGuiTableColumnFlags_WidthFixed,
                            filterWidth + style.ItemSpacing.x + ImGui::CalcTextSize("!").x);
    ImGui::TableNextColumn();
    static const char *tabLabels[] = {"All", "RAW", "Compressed"};
    if (ImGui::BeginTabBar("##filmstripTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      for (int t = 0; t < 3; ++t) {
        if (ImGui::BeginTabItem(tabLabels[t])) {
          app.filmstripTab = t;
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(app.path.empty() || app.isPhotoProtected());
    for (const auto &choice : markChoices) {
      if (choice.mark != PhotoMark::Pick) ImGui::SameLine();
      const bool selected = app.currentPhotoMark == choice.mark;
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
      }
      if (ImGui::SmallButton(choice.label)) assignPhotoMark(app, app.path, choice.mark);
      if (selected) ImGui::PopStyleColor(2);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark current photograph: %s", choice.label);
    }
    ImGui::EndDisabled();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(filterWidth);
    const char *filterLabel = "All";
    for (const auto &choice : markChoices)
      if (app.filmstripMarkFilter == choice.mark) filterLabel = choice.label;
    if (ImGui::BeginCombo("##filmstripMarkFilter", filterLabel)) {
      if (ImGui::Selectable("All", !app.filmstripMarkFilter)) app.filmstripMarkFilter.reset();
      for (const auto &choice : markChoices)
        if (ImGui::Selectable(choice.label, app.filmstripMarkFilter == choice.mark)) app.filmstripMarkFilter = choice.mark;
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark filter");
    if (!app.photoMarkWarning.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("!");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", app.photoMarkWarning.c_str());
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar(2);

  if (app.folderDir.empty() || app.filmstrip.empty()) {
    ImGui::TextDisabled("Open a folder to browse images.");
    return;
  }

  // The focused window scopes future photo actions too; Graph/List keep their
  // own focus and deletion bindings. ImGui maps Ctrl to Command on macOS.
  const auto &io = ImGui::GetIO();
  if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() && !io.WantTextInput &&
      ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
    selectAllVisibleFilmstripEntries(app);

  const float thumbH = std::max(1.0f, ImGui::GetContentRegionAvail().y - style.FramePadding.y * 2.0f);
  const float fbScale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.y);
  const int wantEdge = snapFilmstripThumbEdge(thumbH * fbScale);
  if (app.filmstripThumbEdge.exchange(wantEdge) != wantEdge) invalidateFilmstripThumbs(app);

  pumpFilmstripThumbs(app);
  if (app.filmstripIndex >= 0) requestFilmstripThumb(app, app.filmstripIndex, true);

  for (int i = 0; i < (int)app.filmstrip.size(); ++i) {
    FilmstripEntry &e = app.filmstrip[i];
    if (!filmstripEntryVisible(app, e)) continue;
    const float aspect = (e.th && e.tw) ? (float)e.tw / (float)e.th : 1.0f;
    const ImVec2 btnSize(thumbH * aspect, thumbH);
    ImGui::PushID(i);
    const bool open = i == app.filmstripIndex;
    const bool selected = app.filmstripSelection.count(e.path) != 0;
    if (selected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    }
    if (e.tex)
      ImGui::ImageButton("##t", (ImTextureID)(intptr_t)e.tex, btnSize);
    else
      ImGui::Button(e.thumbFailed ? "?" : "…", btnSize);
    const char *markLabel = "Mark unavailable";
    const char *markIndicator = "?";
    for (const auto &choice : markChoices) {
      if (e.mark == choice.mark) {
        markLabel = choice.label;
        markIndicator = choice.indicator;
      }
    }
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 textPos(min.x + 2.0f, max.y - ImGui::GetTextLineHeight() - 2.0f);
    auto *draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, max, true);
    draw->AddRectFilled(ImVec2(min.x, textPos.y), max, ImGui::GetColorU32(ImGuiCol_WindowBg));
    draw->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), markIndicator);
    draw->PopClipRect();
    if (open)
      draw->AddRect(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1),
                    ImGui::GetColorU32(ImGuiCol_Text), style.FrameRounding, 0, 2);
    if (ImGui::IsItemVisible()) requestFilmstripThumb(app, i, open);
    if (selected) ImGui::PopStyleColor(3);
    if (ImGui::IsItemClicked()) {
      if (io.KeyCtrl || io.KeyShift) {
        selectFilmstripEntry(app, i, io.KeyCtrl, io.KeyShift);
      } else {
        finishUiEdits(app);
        if (openPath(app, e.path, prompt)) {
          selectFilmstripEntry(app, i);
          app.parameterGesture.reset();
        }
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s\n%s", fs::path(e.path).filename().string().c_str(),
                        e.markError.empty() ? markLabel : e.markError.c_str());
    ImGui::PopID();
    if (i + 1 < (int)app.filmstrip.size()) ImGui::SameLine();
  }
}
