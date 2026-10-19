#include "ui/DockLayout.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>

namespace DockLayout {

static void buildDefaultLayout(App &app, ImGuiID dockspaceId) {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspaceId, vp->WorkSize);

  const float refW = std::max(1.0f, vp->WorkSize.x);
  const float refH = std::max(1.0f, vp->WorkSize.y);
  const float leftRatio = std::clamp(app.leftW / refW, 0.12f, 0.40f);
  const float rightRatio = std::clamp(app.rightW / refW, 0.12f, 0.45f);
  const float stripRatio = std::clamp(app.filmstripH / refH, 0.08f, 0.35f);

  ImGuiID dockMain = dockspaceId;
  ImGuiID dockLeft = 0;
  ImGuiID dockRight = 0;
  ImGuiID dockBottom = 0;
  ImGuiID dockNodes = 0;
  ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, leftRatio, &dockLeft, &dockMain);
  ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, rightRatio / (1.0f - leftRatio), &dockRight, &dockMain);
  ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, stripRatio, &dockBottom, &dockMain);
  ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.32f, &dockNodes, &dockMain);

  ImGui::DockBuilderDockWindow(kLeft, dockLeft);
  ImGui::DockBuilderDockWindow(kParams, dockRight);
  ImGui::DockBuilderDockWindow(kFilmstrip, dockBottom);
  ImGui::DockBuilderDockWindow(kNodes, dockNodes);
  ImGui::DockBuilderDockWindow(kPreview, dockMain);
  ImGui::DockBuilderFinish(dockspaceId);
}

static void addNodesToExistingLayout(ImGuiID dockspaceId) {
  // A saved Nodes window (including a floating one) is the user's layout.
  if (ImGui::FindWindowSettingsByID(ImHashStr(kNodes))) return;
  const auto *preview = ImGui::FindWindowSettingsByID(ImHashStr(kPreview));
  auto *dock = preview ? ImGui::DockBuilderGetNode(preview->DockId) : nullptr;
  if (!dock || !dock->IsLeafNode() || ImGui::DockNodeGetRootNode(dock)->ID != dockspaceId) return;
  // Loaded ini nodes have not been laid out yet. Split their saved dimensions,
  // rather than letting the builder clamp a zero-sized child to title-bar height.
  if (dock->Size.x <= 0 || dock->Size.y <= 0) {
    if (dock->SizeRef.x <= 0 || dock->SizeRef.y <= 0) return;
    ImGui::DockBuilderSetNodeSize(dock->ID, dock->SizeRef);
  }
  ImGuiID nodes = 0;
  ImGui::DockBuilderSplitNode(dock->ID, ImGuiDir_Down, 0.32f, &nodes, nullptr);
  ImGui::DockBuilderDockWindow(kNodes, nodes);
  ImGui::DockBuilderFinish(dockspaceId);
}

void BeginMainDockSpace(App &app) {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);

  ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                               ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("##DockHost", nullptr, hostFlags);
  ImGui::PopStyleVar(3);

  const ImGuiID dockspaceId = ImGui::GetID("OfxRawHostDock");
  const bool missingNode = ImGui::DockBuilderGetNode(dockspaceId) == nullptr;
  if (missingNode) {
    buildDefaultLayout(app, dockspaceId);
  } else addNodesToExistingLayout(dockspaceId);

  ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
  ImGui::End();
}

}  // namespace DockLayout
