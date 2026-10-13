#include "ui/NodeList.h"

#include "GraphCommands.h"

#include <algorithm>

ui::NodeListProjection ui::projectNodeList(GraphView graph) {
  NodeListProjection rows;
  rows.spineError = graph::imageSpine(graph, rows.spine);
  for (const auto &node : graph.nodes) {
    if (node.operation == NodeOperation::Source) continue;
    if (std::find(rows.spine.begin(), rows.spine.end(), node.id) == rows.spine.end())
      rows.other.push_back(node.id);
  }
  std::sort(rows.other.begin(), rows.other.end(), [](const NodeId &a, const NodeId &b) {
    return a.value < b.value;
  });
  return rows;
}
