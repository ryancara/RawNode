#pragma once

#include "ui/GraphLayout.h"

namespace ui {

// Stable UI endpoint identity; an empty nodeId names the virtual Output input.
struct GraphPortAddress {
  NodeId nodeId;
  std::optional<SlotId> slot;
  PortId port;
  PortDirection direction;
  bool isDocumentOutput() const { return nodeId.value.empty(); }
};

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
  void cancelInteraction() {
    wire.reset(); dragging.reset(); contextPort.reset();
    panningFromEmptyCanvas = false;
  }
};

void drawGraphCanvas(App &app);

}  // namespace ui
