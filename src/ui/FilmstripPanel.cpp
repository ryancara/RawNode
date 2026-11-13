#include "ui/UiContext.h"
#include "ui/ParamWidgets.h"

#include "persist/DocumentActions.h"
#include "ui/Filmstrip.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

void drawFilmstripPanel(App &app) {
  static const struct {
    const char *label;
    const char *indicator;
    PhotoMark mark;
  } markChoices[] = {{"Pick", "P", PhotoMark::Pick}, {"Unmarked", "U", PhotoMark::Unmarked},
                     {"Reject", "R", PhotoMark::Reject}};
  ImGui::TextUnformatted("Mark current:");
  ImGui::BeginDisabled(app.path.empty() || app.isPhotoProtected());
  for (const auto &choice : markChoices) {
    ImGui::SameLine();
    if (ImGui::SmallButton(choice.label)) assignPhotoMark(app, app.path, choice.mark);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  const char *filterLabel = "All";
  for (const auto &choice : markChoices)
    if (app.filmstripMarkFilter == choice.mark) filterLabel = choice.label;
  if (ImGui::BeginCombo("Mark filter", filterLabel)) {
    if (ImGui::Selectable("All", !app.filmstripMarkFilter)) app.filmstripMarkFilter.reset();
    for (const auto &choice : markChoices)
      if (ImGui::Selectable(choice.label, app.filmstripMarkFilter == choice.mark)) app.filmstripMarkFilter = choice.mark;
    ImGui::EndCombo();
  }
  if (!app.photoMarkWarning.empty()) ImGui::TextWrapped("%s", app.photoMarkWarning.c_str());

  if (app.folderDir.empty() || app.filmstrip.empty()) {
    ImGui::TextDisabled("Open a folder to browse images.");
    return;
  }

  static const char *tabLabels[] = {"All", "RAW", "Compressed"};
  if (ImGui::BeginTabBar("##filmstripTabs")) {
    for (int t = 0; t < 3; ++t) {
      if (ImGui::BeginTabItem(tabLabels[t])) {
        app.filmstripTab = t;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  const float thumbH = std::max(24.0f, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().FramePadding.y * 2.0f);
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
