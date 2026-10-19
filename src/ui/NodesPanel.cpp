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
  ImGui::Separator();
  if (app.nodesView == NodesView::Graph) ui::drawGraphCanvas(app);
  else {
    app.graphCanvas.dragging.reset();
    ui::drawNodeList(app);
  }
}
