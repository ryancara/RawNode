#pragma once

#include "GraphModel.h"

struct App;
namespace ui {
// One active Inspector item. Only numeric text inputs defer a value; live
// sliders/strings need identity and ImGui lifetime, not a second grade snapshot.
struct ParameterGesture {
  unsigned item = 0;
  std::string photoPath;
  NodeId node;
  std::string parameter;
  std::optional<ParameterValue> pendingValue;
};
}

void drawNodeOpacity(App &app, Node &node);
void drawParams(App &app, Node &node, const std::string &parent);
// Called after drawing, or explicitly before navigation/discrete actions.
// A vanished/disabled item completes its actual live edit. Export defers capture.
// True means a full capture was attempted, including failure. A combined
// position completion can reuse that attempt instead of capturing twice.
bool finishParameterGesture(App &app, bool force = false);
