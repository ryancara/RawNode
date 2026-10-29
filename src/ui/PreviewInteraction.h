#pragma once

#include "GraphModel.h"
#include "imgui.h"

#include <array>

struct App;

namespace ui {

// The displayed photo rectangle already includes fit, zoom and pan. Image
// coordinates use the Radial contract's bottom-left origin, independent of size.
struct PreviewTransform {
  ImVec2 origin, size;
  ImVec2 toScreen(double x, double y) const;
  std::array<double, 2> toImage(ImVec2 point) const;
};

enum class PreviewDrag { None, Ignore, Pan, Centre, Left, Right, Top, Bottom, Feather };
struct RadialGeometry {
  double centreX = 0, centreY = 0, radiusX = 0, radiusY = 0, feather = 0;
};
struct RadialHandle {
  PreviewDrag kind;
  ImVec2 anchor, grip;
};
using RadialHandles = std::array<RadialHandle, 6>;
RadialHandles radialHandles(const PreviewTransform &view, const RadialGeometry &geometry);
PreviewDrag hitRadialHandle(const RadialHandles &handles, ImVec2 point);
bool radialHandlesAligned(const App &app);

// Only an in-progress gesture's baseline, never saved geometry or processor
// ownership. Live values always come from the selected processor parameters.
struct PreviewInteraction {
  PreviewDrag drag = PreviewDrag::None;
  int button = -1;
  ImGuiID canvas = 0;
  std::string photoPath;
  std::optional<NodeId> node;
  std::array<double, 2> startPointer{};
  RadialGeometry start;
  bool editPending = false;
  void stop() { drag = PreviewDrag::None; }
};

// Called for the actual Preview canvas item. Returns whether this gesture owns
// a double click, so the ordinary background zoom reset can remain unchanged.
bool drawPreviewInteraction(App &app, const PreviewTransform &view, bool hovered,
                            bool active, bool textInputWasActive);
bool finishPreviewInteraction(App &app, bool force = false);

}  // namespace ui
