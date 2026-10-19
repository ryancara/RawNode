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
};

void drawGraphCanvas(App &app);

}  // namespace ui
