#include "ui/UiContext.h"
#include "ui/ParamWidgets.h"

#include "persist/DocumentActions.h"
#include "ui/Filmstrip.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

void drawFilmstripPanel(App &app) {
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
    const bool selected = i == app.filmstripIndex;
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
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
    if (ImGui::IsItemVisible()) requestFilmstripThumb(app, i, selected);
    if (selected) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked()) {
      finishUiEdits(app);
      if (openPath(app, e.path, promptSaveFailure)) app.parameterGesture.reset();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s\n%s", fs::path(e.path).filename().string().c_str(),
                        e.markError.empty() ? markLabel : e.markError.c_str());
    ImGui::PopID();
    if (i + 1 < (int)app.filmstrip.size()) ImGui::SameLine();
  }
}
