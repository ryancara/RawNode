#pragma once

#include "ui/GraphLayout.h"

namespace ui {

// UI-thread gestures/viewport only; never serialized with the photograph.
struct GraphCanvasState {
  GraphViewport view;
  std::string photoPath;
  bool framePending = true;
  PersistPosition lastCanvasSize;
  std::optional<NodeId> dragging;
  bool panningFromEmptyCanvas = false;
  PersistPosition dragOffset;
  std::optional<GraphPortAddress> wire;
  std::optional<GraphPortAddress> contextPort;
  std::optional<NodeId> contextNode;
  void cancelInteraction() {
    wire.reset(); dragging.reset(); contextPort.reset(); contextNode.reset();
    panningFromEmptyCanvas = false;
  }
};

void drawGraphCanvas(App &app);

}  // namespace ui
