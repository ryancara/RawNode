#include "ui/UiContext.h"

#include "ui/GraphCanvas.h"
#include "ui/NodeList.h"

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
  ImGui::Separator();
  if (app.nodesView == NodesView::Graph) ui::drawGraphCanvas(app);
  else {
    app.graphCanvas.cancelInteraction();
    ui::finishGraphDrag(app);
    ui::drawNodeList(app);
  }
}
