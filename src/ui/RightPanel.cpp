#include "ui/UiContext.h"

#include "NodeGraph.h"
#include "ui/ParamWidgets.h"
#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

void drawRightPanel(App &app) {
  Node *node = selectedNode(app);
  if (!node) {
    ImGui::TextDisabled("Select a node to edit parameters.");
    return;
  }
  ImGui::AlignTextToFramePadding();
  const std::string label = nodeDisplayName(*node);
  ImGui::TextUnformatted(label.c_str());
  ImGui::Separator();
  if (node->operation != NodeOperation::ImageProcessor) {
    ImGui::TextDisabled("No parameter controls are available for this graph operation.");
    return;
  }
  if (!node->processor) {
    ImGui::TextDisabled("Processor unavailable. Its saved state is preserved and the node is bypassed.");
    return;
  }
  // ImGui edit/focus state belongs to the logical node, even when two nodes
  // have the same processor and parameter names.
  ImGui::PushID(node->id.value.data(), node->id.value.data() + node->id.value.size());
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F) || ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_F))
    ImGui::SetKeyboardFocusHere();
  const bool hasFilter = app.paramFilter[0] != '\0';
  if (hasFilter) {
    const float clearW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW - ImGui::GetStyle().ItemSpacing.x);
  } else {
    ImGui::SetNextItemWidth(-1);
  }
  ImGui::InputTextWithHint("##paramFilter", "Search parameters...", app.paramFilter, sizeof app.paramFilter);
  if (hasFilter) {
    ImGui::SameLine();
    if (iconBtn("##clearFilter", ICON_FA_XMARK)) app.paramFilter[0] = '\0';
  }
  ImGui::Separator();
  drawParams(app, *node, "");
  ImGui::PopID();
}
