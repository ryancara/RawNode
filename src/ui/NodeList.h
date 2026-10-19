#pragma once

#include "GraphModel.h"

struct App;

namespace ui {

// Ephemeral rows, derived from the live graph. Neither section defines an
// execution order or changes connections. Other nodes use NodeId display order.
struct NodeListProjection {
  std::vector<NodeId> spine;
  std::vector<NodeId> other;
  // Traversal failures retain a unary suffix; structural failures leave all
  // non-Source nodes in other. Either failure disables spine moves.
  std::string spineError;
};

NodeListProjection projectNodeList(GraphView graph);
void drawNodeList(App &app);

}  // namespace ui
