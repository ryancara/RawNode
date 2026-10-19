#include "ui/GraphLayout.h"

#include "AppState.h"
#include "NodeGraph.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace ui {
namespace {
constexpr double columnSpacing = graphNodeWidth + 80;
constexpr double rowGap = 40;

bool finite(PersistPosition position) { return std::isfinite(position.x) && std::isfinite(position.y); }

GraphNodeLayout nodeGeometry(const Node &node) {
  GraphNodeLayout layout{&node, {}, 0, {}};
  size_t inputs = 0, outputs = 0;
  const auto port = [&](const PortDeclaration &declaration, std::optional<SlotId> slot) {
    const bool input = declaration.direction == PortDirection::Input;
    const size_t row = input ? inputs++ : outputs++;
    layout.ports.push_back({&declaration, slot,
        {input ? 0.0 : graphNodeWidth, graphHeaderHeight + (row + 0.5) * graphPortSpacing}});
  };
  const auto &interface = nodeInterface(node.operation);
  for (const auto &declaration : interface.fixedPorts) port(declaration, std::nullopt);
  for (const auto &slot : node.slots)
    for (const auto &declaration : interface.slotInputs) port(declaration, slot.id);
  layout.height = graphHeaderHeight + std::max(inputs, outputs) * graphPortSpacing + 24;
  return layout;
}

// Iterative longest dependency depth. Only temporary layout adjacency exists;
// it is discarded before returning, and never edits/evaluates the graph.
std::map<std::string, size_t> dependencyColumns(const GraphLayout &layout) {
  std::map<std::string, size_t> columns, incoming;
  std::map<std::string, std::vector<std::string>> consumers;
  std::set<std::string> ready;
  for (const auto &entry : layout.nodes) {
    const Node &node = *entry.second.node;
    columns[entry.first] = node.operation == NodeOperation::Source ? 0 : 1;
    incoming[entry.first] = 0;
    const auto bindings = [&](const InputBindings &inputs) {
      for (const auto &input : inputs) {
        if (!input.second || !layout.nodes.count(input.second->nodeId.value)) continue;
        ++incoming[entry.first];
        consumers[input.second->nodeId.value].push_back(entry.first);
      }
    };
    bindings(node.fixedInputs);
    for (const auto &slot : node.slots) bindings(slot.inputs);
    if (!incoming[entry.first]) ready.insert(entry.first);
  }
  while (!ready.empty()) {
    const std::string id = *ready.begin();
    ready.erase(ready.begin());
    for (const auto &consumer : consumers[id]) {
      columns[consumer] = std::max(columns[consumer], columns[id] + 1);
      if (--incoming[consumer] == 0) ready.insert(consumer);
    }
  }
  return columns;
}

bool overlaps(const GraphNodeLayout &a, const GraphNodeLayout &b) {
  return a.position.x < b.position.x + graphNodeWidth + rowGap &&
         a.position.x + graphNodeWidth + rowGap > b.position.x &&
         a.position.y < b.position.y + b.height + rowGap &&
         a.position.y + a.height + rowGap > b.position.y;
}
}  // namespace

GraphLayout layoutGraph(GraphView graph, const PersistPositions &saved) {
  GraphLayout layout;
  for (const auto &node : graph.nodes) layout.nodes.emplace(node.id.value, nodeGeometry(node));
  const auto columns = dependencyColumns(layout);
  std::vector<const GraphNodeLayout *> placed;
  std::vector<std::string> missing;
  for (auto &entry : layout.nodes) {
    const auto found = saved.find(entry.first);
    if (found != saved.end() && finite(found->second)) {
      entry.second.position = found->second;
      placed.push_back(&entry.second);
    } else missing.push_back(entry.first);
  }
  std::sort(missing.begin(), missing.end(), [&](const auto &a, const auto &b) {
    return columns.at(a) == columns.at(b) ? a < b : columns.at(a) < columns.at(b);
  });
  for (const auto &id : missing) {
    auto &node = layout.nodes.at(id);
    node.position = {columns.at(id) * columnSpacing, 0};
    // Saved nodes are never moved. Missing nodes step below occupied rectangles.
    bool collision;
    do {
      collision = false;
      for (const auto *other : placed) {
        if (!overlaps(node, *other)) continue;
        node.position.y = other->position.y + other->height + rowGap;
        collision = true;
      }
    } while (collision);
    placed.push_back(&node);
  }
  double right = 0;
  for (const auto &entry : layout.nodes)
    right = std::max(right, entry.second.position.x + graphNodeWidth);
  const auto output = layout.nodes.find(graph.documentOutput.nodeId.value);
  layout.output = {right + 80, output == layout.nodes.end() ? 0 : output->second.position.y};
  return layout;
}

bool moveGraphNode(App &app, const NodeId &id, PersistPosition position) {
  if (app.isPhotoProtected() || !finite(position) || !findNode(app, id)) return false;
  // Control-thread layout data only. The existing grade-save lifecycle captures
  // it; no DocumentMutation, render request or immediate save belongs here.
  app.nodePositions[id.value] = position;
  return true;
}

PersistPosition graphToCanvas(const GraphViewport &view, PersistPosition position) {
  return {position.x * view.zoom + view.pan.x, position.y * view.zoom + view.pan.y};
}
PersistPosition canvasToGraph(const GraphViewport &view, PersistPosition position) {
  return {(position.x - view.pan.x) / view.zoom, (position.y - view.pan.y) / view.zoom};
}
void zoomGraphAt(GraphViewport &view, PersistPosition cursor, double factor) {
  if (!std::isfinite(factor) || factor <= 0) return;
  const auto anchor = canvasToGraph(view, cursor);
  view.zoom = std::clamp(view.zoom * factor, graphMinZoom, graphMaxZoom);
  view.pan = {cursor.x - anchor.x * view.zoom, cursor.y - anchor.y * view.zoom};
}
void frameGraph(GraphViewport &view, const GraphLayout &layout, PersistPosition size) {
  if (size.x <= 0 || size.y <= 0) return;
  PersistPosition low = layout.output;
  PersistPosition high{low.x + graphOutputWidth, low.y + graphOutputHeight};
  for (const auto &entry : layout.nodes) {
    const auto &node = entry.second;
    low.x = std::min(low.x, node.position.x); low.y = std::min(low.y, node.position.y);
    high.x = std::max(high.x, node.position.x + graphNodeWidth);
    high.y = std::max(high.y, node.position.y + node.height);
  }
  constexpr double padding = 32;
  view.zoom = std::clamp(std::min({1.0, std::max(1.0, size.x - 2 * padding) / (high.x - low.x),
                                      std::max(1.0, size.y - 2 * padding) / (high.y - low.y)}),
                         graphMinZoom, graphMaxZoom);
  view.pan = {size.x * 0.5 - (low.x * 0.5 + high.x * 0.5) * view.zoom,
              size.y * 0.5 - (low.y * 0.5 + high.y * 0.5) * view.zoom};
}
}  // namespace ui
