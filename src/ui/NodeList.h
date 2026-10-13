#pragma once

#include "GraphModel.h"

namespace ui {

// Ephemeral rows, derived from the live graph. Neither section defines an
// execution order or changes connections. Other nodes use NodeId display order.
struct NodeListProjection {
  std::vector<NodeId> spine;
  std::vector<NodeId> other;
  std::string spineError;  // A partial spine remains visible, but cannot move.
};

NodeListProjection projectNodeList(GraphView graph);

}  // namespace ui
