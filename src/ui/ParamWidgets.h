#pragma once

#include "GraphModel.h"

struct App;
namespace ui {
// One parameter edit in the Inspector or Preview. Only numeric text fields
// defer their value.
struct ParameterGesture {
  unsigned item = 0;
  std::string photoPath;
  NodeId node;
  std::string parameter;
  std::optional<ParameterValue> pendingValue;
  bool nodeStrength = false; // Node control, never a backend parameter with the same ID.
  bool heldAdjustment = false;
};
}

void drawNodeOpacity(App &app, Node &node);
void drawParams(App &app, Node &node, const std::string &parent);
// Called after drawing, or explicitly before navigation/discrete actions.
// A vanished/disabled item completes its actual live edit. Export defers capture.
// True means a full capture was attempted, including failure. A combined
// position completion can reuse that attempt instead of capturing twice.
bool finishParameterGesture(App &app, bool force = false);
