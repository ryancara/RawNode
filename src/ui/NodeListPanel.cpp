#include "ui/NodeList.h"

#include "AppState.h"
#include "NodeGraph.h"
#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>

void ui::drawNodeList(App &app) {
  ImGui::BeginChild("nodeList", ImVec2(0, 0), ImGuiChildFlags_Borders);
  const auto &io = ImGui::GetIO();
  // ImGui resets keyboard-mode hover suppression when navigation is disabled.
  auto *listState = ImGui::GetStateStorage();
  const auto hoverSuppressionId = ImGui::GetID("##postDeleteHover");
  bool suppressHover = listState->GetBool(hoverSuppressionId);
  if (suppressHover && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 ||
      ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
      (ImGui::IsWindowFocused() && ImGui::GetCurrentContext()->NavId != 0))) {
    suppressHover = false;
    listState->SetBool(hoverSuppressionId, false);
  }
  if (suppressHover) ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
  if (ImGui::Selectable("Source", app.selectedNodeId() == app.primarySource))
    selectNode(app, app.primarySource);
  const auto rows = ui::projectNodeList({app.nodes, app.primarySource, app.documentOutput});
  if (const Node *output = findNode(app, app.documentOutput.nodeId))
    ImGui::TextWrapped("Document output: %s", nodeDisplayName(*output).c_str());
  if (!rows.spineError.empty())
    ImGui::TextWrapped("Reordering unavailable: %s. All graph nodes are shown below.", rows.spineError.c_str());
  if (rows.spine.empty() && rows.other.empty())
    ImGui::TextDisabled("No nodes yet.\nAdd a processor to build a chain.");
  const float btnH = ImGui::GetFrameHeight();
  const float btnGap = ImGui::GetStyle().ItemSpacing.x;
  const float btnsW = 4.0f * btnH + 3.0f * btnGap;
  std::optional<NodeId> remove, moveFrom, moveTo, enable;
  bool enabled = false;
  const auto drawRows = [&](const std::vector<NodeId> &ids, bool mainSpine) {
    const bool reorderable = !app.isPhotoProtected() && mainSpine && rows.spineError.empty();
    for (size_t row = 0; row < ids.size(); ++row) {
      const Node *node = findNode(app, ids[row]);
      if (!node) continue;
      ImGui::PushID(node->id.value.data(), node->id.value.data() + node->id.value.size());
      const std::string label = nodeDisplayName(*node);
      if (!node->enabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
      if (ImGui::Selectable(label.c_str(), app.selectedNodeId() == node->id, 0,
                            ImVec2(std::max(btnH, ImGui::GetContentRegionAvail().x - btnsW - btnGap), btnH)))
        selectNode(app, node->id);
      if (!node->enabled) ImGui::PopStyleVar();
      if (reorderable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("NODE_ID", node->id.value.c_str(), node->id.value.size() + 1);
        ImGui::Text("%s", label.c_str());
        ImGui::EndDragDropSource();
      }
      if (reorderable && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("NODE_ID")) {
          const auto *data = static_cast<const char *>(payload->Data);
          if (payload->DataSize > 1 && data[payload->DataSize - 1] == '\0') {
            moveFrom = NodeId{std::string(data, payload->DataSize - 1)};
            moveTo = node->id;
          }
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::SameLine(0.0f, btnGap);
      ImGui::BeginDisabled(app.isPhotoProtected());
      if (iconBtn("##en", node->enabled ? ICON_FA_EYE : ICON_FA_EYE_SLASH)) {
        enable = node->id;
        enabled = !node->enabled;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip(node->enabled ? "Disable processing" : "Enable processing");

      const auto arrow = [&](const char *id, const char *icon, bool available, const char *hint) {
        ImGui::SameLine(0.0f, btnGap);
        ImGui::BeginDisabled(!available);
        const bool clicked = iconBtn(id, icon);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
          ImGui::SetTooltip("%s", !mainSpine ? "Only main Image spine nodes can be moved" :
                            !reorderable ? "Moves require a complete main Image spine" : hint);
        return clicked;
      };
      if (arrow("##up", ICON_FA_ARROW_UP, reorderable && row > 0,
                row > 0 ? "Move up" : "Already first on the main Image spine")) {
        moveFrom = node->id;
        moveTo = ids[row - 1];
      }
      if (arrow("##dn", ICON_FA_ARROW_DOWN, reorderable && row + 1 < ids.size(),
                row + 1 < ids.size() ? "Move down" : "Already last on the main Image spine")) {
        moveFrom = node->id;
        moveTo = ids[row + 1];
      }
      ImGui::SameLine(0.0f, btnGap);
      if (iconBtn("##rm", ICON_FA_XMARK)) remove = node->id;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Remove node");
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  };
  if (!rows.spine.empty()) {
    ImGui::SeparatorText(rows.spineError.empty() ? "Main Image spine" : "Main Image spine (partial)");
    drawRows(rows.spine, true);
  }
  if (!rows.other.empty()) {
    ImGui::SeparatorText("Other graph nodes");
    ImGui::TextDisabled("Outside the displayed Image spine;\nselect and edit without list reordering.");
    drawRows(rows.other, false);
  }
  ImGui::Separator();
  if (ImGui::Selectable("Output", app.outputSelected())) selectOutput(app);
  if (suppressHover) ImGui::PopStyleColor();
  const bool keyboardRemove = ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() &&
      !io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift &&
      app.renderer.canEditParameters() && selectedNode(app) &&
      (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
  if (keyboardRemove) remove = app.selectedNodeId();
  ImGui::EndChild();
  // Apply one row-control intent after all references into the node list have
  // expired. Intents and drag/drop retain identity, never storage positions.
  if (remove) {
    if (destroyNode(app, *remove) && keyboardRemove) {
      // The next row moves under a stationary pointer. Hide that hover fill until
      // deliberate mouse/focus input, keeping selection and Inspector neutral.
      listState->SetBool(hoverSuppressionId, true);
      auto &gui = *ImGui::GetCurrentContext();
      ImGui::SetNavID(0, gui.NavLayer, gui.NavFocusScopeId, ImRect());
    }
  } else if (moveFrom && moveTo) moveNode(app, *moveFrom, *moveTo);
  else if (enable) setNodeEnabled(app, *enable, enabled);
}
