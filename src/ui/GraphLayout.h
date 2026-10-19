#pragma once

#include "GraphModel.h"
#include "persist/ProjectPersist.h"

struct App;

namespace ui {

inline constexpr double graphNodeWidth = 240;
inline constexpr double graphHeaderHeight = 32;
inline constexpr double graphPortSpacing = 24;
inline constexpr double graphOutputWidth = 140;
inline constexpr double graphOutputHeight = 80;
inline constexpr double graphMinZoom = 0.2;
inline constexpr double graphMaxZoom = 2.5;

struct GraphPortLayout {
  const PortDeclaration *declaration;
  std::optional<SlotId> slot;
  PersistPosition offset;
};

// Frame-local drawing geometry borrowing the authoritative nodes/interfaces.
// No connection records: drawing reads the consumer bindings directly.
struct GraphNodeLayout {
  const Node *node;
  PersistPosition position;
  double height;
  std::vector<GraphPortLayout> ports;
};
struct GraphLayout {
  std::map<std::string, GraphNodeLayout> nodes;
  PersistPosition output;
};

// Stable gesture/menu identity. Empty nodeId identifies the virtual Output input.
struct GraphPortAddress {
  NodeId nodeId;
  std::optional<SlotId> slot;
  PortId port;
  PortDirection direction;
  bool isDocumentOutput() const { return nodeId.value.empty(); }
};
struct GraphPortHit {
  GraphPortAddress address;
  PersistPosition position;
  ValueType type;
};

GraphLayout layoutGraph(GraphView graph, const PersistPositions &saved);
bool moveGraphNode(App &app, const NodeId &id, PersistPosition position);

// Pan is in canvas-local pixels; positions are in graph coordinates.
struct GraphViewport {
  PersistPosition pan{32, 32};
  double zoom = 1;
};
PersistPosition graphToCanvas(const GraphViewport &view, PersistPosition position);
PersistPosition canvasToGraph(const GraphViewport &view, PersistPosition position);
void zoomGraphAt(GraphViewport &view, PersistPosition cursor, double factor);
void frameGraph(GraphViewport &view, const GraphLayout &layout, PersistPosition canvasSize);
// Canvas-local screen pixels. Nearest eligible port wins; vertical hit areas
// shrink with row spacing so neighbouring ports remain unambiguous at low zoom.
std::optional<GraphPortHit> hitGraphPort(const GraphLayout &layout, const GraphViewport &view,
                                      PersistPosition point);

}  // namespace ui
