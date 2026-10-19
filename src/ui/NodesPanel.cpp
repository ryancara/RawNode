#include "ui/UiContext.h"

#include "ui/GraphCanvas.h"
#include "ui/NodeList.h"
#include "NodeGraph.h"

#include "imgui.h"

void drawNodesPanel(App &app) {
  if (ImGui::RadioButton("List", app.nodesView == NodesView::List)) app.nodesView = NodesView::List;
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (ImGui::RadioButton("Graph", app.nodesView == NodesView::Graph)) app.nodesView = NodesView::Graph;
  const float addWidth = ImGui::CalcTextSize("+ Add Processor").x + 2 * ImGui::GetStyle().FramePadding.x;
  const float used = ImGui::GetItemRectMax().x - ImGui::GetCursorScreenPos().x;
  if (ImGui::GetContentRegionAvail().x - used >= addWidth + ImGui::GetStyle().ItemSpacing.x)
    ImGui::SameLine();
  drawProcessorPicker(app);
  if (selectedNode(app)) {
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.renderer.canEditParameters());
    if (ImGui::Button("Delete Selected Node")) {
      const auto id = app.selectedNodeId();
      if (id) destroyNode(app, *id);
    }
    ImGui::EndDisabled();
  }
  ImGui::Separator();
  if (app.nodesView == NodesView::Graph) ui::drawGraphCanvas(app);
  else {
    app.graphCanvas.cancelInteraction();
    ui::drawNodeList(app);
  }
}
