#include "ui/PreviewInteraction.h"

#include "AppState.h"
#include "NodeGraph.h"
#include "processors/NativeCstProcessor.h"
#include "processors/NativeExposureProcessor.h"
#include "processors/NativeRadialMaskProcessor.h"
#include "ui/ParamWidgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {
bool isHandle(PreviewDrag drag) { return drag >= PreviewDrag::Centre; }

RadialGeometry geometry(const Node &node) {
  RadialGeometry value;
  for (const auto &parameter : node.processor->parameters()) {
    const double v = std::get<double>(parameter.value);
    if (parameter.id == "centreX") value.centreX = v;
    else if (parameter.id == "centreY") value.centreY = v;
    else if (parameter.id == "radiusX") value.radiusX = v;
    else if (parameter.id == "radiusY") value.radiusY = v;
    else if (parameter.id == "feather") value.feather = v;
  }
  return value;
}

// Only cross operations whose implementation establishes an unchanged raster
// frame. Equal dimensions alone do not establish alignment through OFX/CTL.
// Exact bypasses use their declared pass-through, as the evaluator does.
std::optional<OutputRef> rasterFrame(const App &app, OutputRef output) {
  for (size_t depth = 0; depth <= app.nodes.size(); ++depth) {
    const Node *node = findNode(app, output.nodeId);
    const auto *port = findOutput(app, output);
    if (!node || !port || port->type != ValueType::Image) return std::nullopt;
    if (node->operation == NodeOperation::Source) return output;
    const bool bypass = !node->enabled || (node->operation == NodeOperation::ImageProcessor && node->strength == 0);
    const bool native = dynamic_cast<const NativeExposureProcessor *>(node->processor.get()) ||
                        dynamic_cast<const NativeCstProcessor *>(node->processor.get());
    if ((!bypass && !native) || !port->bypassInput) return output;
    const auto *input = findInput(app, {node->id, std::nullopt, *port->bypassInput});
    if (!input || !*input) return std::nullopt;
    output = **input;
  }
  return std::nullopt; // A malformed cycle must never hang the UI.
}

void editGeometry(App &app, const PreviewTransform &view) {
  const auto &state = app.previewInteraction;
  const auto pointer = view.toImage(ImGui::GetIO().MousePos);
  const double dx = pointer[0] - state.startPointer[0], dy = pointer[1] - state.startPointer[1];
  const auto &g = state.start;
  const auto current = geometry(*selectedNode(app));
  const auto set = [&](const char *id, double value, double previous, double lo, double hi) {
    value = std::clamp(value, lo, hi);
    if (value != previous && setNodeParameterValue(app, *state.node, id, value))
      app.previewInteraction.editPending = true;
  };
  switch (state.drag) {
    case PreviewDrag::Centre:
      set("centreX", g.centreX + dx, current.centreX, -1, 2);
      set("centreY", g.centreY + dy, current.centreY, -1, 2);
      break;
    case PreviewDrag::Left: case PreviewDrag::Right:
      set("radiusX", g.radiusX + (state.drag == PreviewDrag::Right ? dx : -dx), current.radiusX, 0, 2);
      break;
    case PreviewDrag::Top: case PreviewDrag::Bottom:
      set("radiusY", g.radiusY + (state.drag == PreviewDrag::Top ? dy : -dy), current.radiusY, 0, 2);
      break;
    case PreviewDrag::Feather: {
      // Project displacement onto the inward boundary's diagonal spoke. This
      // also works when one radius is zero, without dividing by that radius.
      constexpr double diagonal = 0.7071067811865475244;
      const double sx = g.radiusX * view.size.x * diagonal;
      const double sy = g.radiusY * view.size.y * diagonal;
      const double length2 = sx * sx + sy * sy;
      if (length2 > 0) {
        const double delta = (dx * view.size.x * sx + dy * view.size.y * sy) / length2;
        set("feather", g.feather - delta, current.feather, 0, 1);
      }
      break;
    }
    default: break;
  }
}

void drawGeometry(const PreviewTransform &view, const RadialGeometry &g, const RadialHandles &handles,
                  PreviewDrag highlight) {
  auto *draw = ImGui::GetWindowDrawList();
  const auto centre = view.toScreen(g.centreX, g.centreY);
  const ImVec2 outer{float(g.radiusX * view.size.x), float(g.radiusY * view.size.y)};
  const ImU32 black = IM_COL32(0, 0, 0, 230), white = IM_COL32(255, 255, 255, 230);
  const auto ellipse = [&](double scale, ImU32 colour) {
    const ImVec2 radius{float(outer.x * scale), float(outer.y * scale)};
    if (radius.x < 0.5f || radius.y < 0.5f) {
      const ImVec2 a{centre.x - radius.x, centre.y - radius.y}, b{centre.x + radius.x, centre.y + radius.y};
      draw->AddLine(a, b, black, 3); draw->AddLine(a, b, colour, 1);
    } else {
      const int segments = int(std::clamp(std::max(radius.x, radius.y), 48.0f, 512.0f));
      draw->AddEllipse(centre, radius, black, 0, segments, 3);
      draw->AddEllipse(centre, radius, colour, 0, segments, 1);
    }
  };
  ellipse(1, white);
  ellipse(1 - g.feather, IM_COL32(200, 200, 200, 220));
  for (const auto &handle : handles) {
    const auto p = handle.grip;
    draw->AddLine(handle.anchor, p, black, 3);
    draw->AddLine(handle.anchor, p, white, 1);
    const ImU32 fill = handle.kind == highlight ? ImGui::GetColorU32(ImGuiCol_PlotHistogram) : white;
    if (handle.kind == PreviewDrag::Centre) {
      draw->AddCircleFilled(p, 6, black); draw->AddCircleFilled(p, 4, fill);
    } else if (handle.kind == PreviewDrag::Feather) {
      const ImVec2 diamond[] = {{p.x, p.y - 6}, {p.x + 6, p.y}, {p.x, p.y + 6}, {p.x - 6, p.y}};
      draw->AddConvexPolyFilled(diamond, 4, fill); draw->AddPolyline(diamond, 4, black, ImDrawFlags_Closed, 2);
    } else {
      draw->AddRectFilled({p.x - 5, p.y - 5}, {p.x + 5, p.y + 5}, fill);
      draw->AddRect({p.x - 5, p.y - 5}, {p.x + 5, p.y + 5}, black, 0, 0, 2);
    }
  }
}
}

ImVec2 PreviewTransform::toScreen(double x, double y) const {
  return {float(origin.x + x * size.x), float(origin.y + (1 - y) * size.y)};
}
std::array<double, 2> PreviewTransform::toImage(ImVec2 point) const {
  return {(double(point.x) - origin.x) / size.x, 1 - (double(point.y) - origin.y) / size.y};
}

RadialHandles radialHandles(const PreviewTransform &view, const RadialGeometry &g) {
  constexpr double diagonal = 0.7071067811865475244;
  RadialHandles handles{{
      {PreviewDrag::Centre, view.toScreen(g.centreX, g.centreY), {}},
      {PreviewDrag::Left, view.toScreen(g.centreX - g.radiusX, g.centreY), {}},
      {PreviewDrag::Right, view.toScreen(g.centreX + g.radiusX, g.centreY), {}},
      {PreviewDrag::Top, view.toScreen(g.centreX, g.centreY + g.radiusY), {}},
      {PreviewDrag::Bottom, view.toScreen(g.centreX, g.centreY - g.radiusY), {}},
      {PreviewDrag::Feather, view.toScreen(g.centreX + g.radiusX * (1 - g.feather) * diagonal,
                                        g.centreY + g.radiusY * (1 - g.feather) * diagonal), {}}}};
  // Screen-sized grips remain individually reachable when geometry collapses.
  // Leader lines retain the exact parameter positions; offsets never edit it.
  for (size_t i = 0; i < handles.size(); ++i) {
    auto &handle = handles[i];
    handle.grip = handle.anchor;
    for (int candidate = 0; candidate < 49; ++candidate) {
      const int ring = (candidate + 7) / 8, direction = (candidate - 1) % 8;
      constexpr ImVec2 offsets[] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}, {1, -1}, {-1, -1}, {-1, 1}, {1, 1}};
      const ImVec2 offset = candidate == 0 ? ImVec2{} : offsets[direction];
      const ImVec2 point{handle.anchor.x + offset.x * ring * 18, handle.anchor.y + offset.y * ring * 18};
      bool free = true;
      for (size_t previous = 0; previous < i; ++previous)
        if (std::hypot(point.x - handles[previous].grip.x, point.y - handles[previous].grip.y) < 16) free = false;
      if (free) { handle.grip = point; break; }
    }
  }
  return handles;
}

PreviewDrag hitRadialHandle(const RadialHandles &handles, ImVec2 point) {
  PreviewDrag hit = PreviewDrag::None;
  float distance = 8;
  for (const auto &handle : handles) {
    const float d = std::hypot(point.x - handle.grip.x, point.y - handle.grip.y);
    if (d < distance) { distance = d; hit = handle.kind; }
  }
  return hit;
}

bool radialHandlesAligned(const App &app) {
  const auto *node = selectedNode(app);
  if (!node || node->operation != NodeOperation::NativeRadialMask ||
      !dynamic_cast<const NativeRadialMaskProcessor *>(node->processor.get()) ||
      !app.tex || app.texW <= 0 || app.texH <= 0 || app.full.px.empty()) return false;
  if (previewMode(app) == PreviewMode::MaskOnly) return true;
  const auto *binding = findInput(app, {node->id, std::nullopt, ports::reference});
  const OutputRef reference = binding && *binding ? **binding : OutputRef{app.primarySource, ports::image};
  const auto referenceFrame = rasterFrame(app, reference), photoFrame = rasterFrame(app, app.documentOutput);
  return referenceFrame && photoFrame && *referenceFrame == *photoFrame;
}

bool finishPreviewInteraction(App &app, bool force) {
  auto &state = app.previewInteraction;
  if (state.photoPath != app.path ||
      (app.isPhotoProtected() && (isHandle(state.drag) || state.editPending))) { state = {}; return false; }
  const auto *context = ImGui::GetCurrentContext();
  if (force || !context || ImGui::GetIO().AppFocusLost || !ImGui::IsMousePosValid() ||
      !ImGui::IsMouseDown(state.button < 0 ? 0 : state.button) ||
      context->ActiveId != state.canvas || context->ActiveIdIsAlive != state.canvas ||
      (isHandle(state.drag) && (state.node != app.selectedNodeId() ||
                               !app.renderer.canEditParameters() || !radialHandlesAligned(app)))) state.stop();
  if (state.drag != PreviewDrag::None || !state.editPending || !app.renderer.canCaptureSnapshot()) return false;
  state.editPending = false; // A failed capture leaves GradeHistory's pending edit intact.
  return app.history.recordCompletedEdit(app);
}

bool drawPreviewInteraction(App &app, const PreviewTransform &view, bool hovered,
                            bool active, bool textInputWasActive) {
  auto &state = app.previewInteraction;
  const auto &io = ImGui::GetIO();
  const bool eligible = radialHandlesAligned(app) && app.renderer.canEditParameters();
  const auto hit = eligible ? hitRadialHandle(radialHandles(view, geometry(*selectedNode(app))), io.MousePos) : PreviewDrag::None;
  // Finish outgoing state before accepting a new press, including selection and
  // export interruptions. ActiveIdIsAlive is supplied by the real canvas item.
  finishPreviewInteraction(app);
  if (active && hovered && state.drag == PreviewDrag::None && !state.editPending && ImGui::IsItemActivated()) {
    const bool middle = ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    const bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !textInputWasActive &&
                       !io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt;
    finishParameterGesture(app, true);
    state.photoPath = app.path;
    state.canvas = ImGui::GetItemID();
    state.button = middle ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left;
    state.drag = middle || space ? PreviewDrag::Pan : hit == PreviewDrag::None ? PreviewDrag::Ignore : hit;
    state.node = app.selectedNodeId();
    state.startPointer = view.toImage(io.MousePos);
    if (isHandle(state.drag)) state.start = geometry(*selectedNode(app));
  }
  if (state.drag == PreviewDrag::Pan && ImGui::IsMouseDragging(state.button)) {
    app.previewPanX += io.MouseDelta.x; app.previewPanY += io.MouseDelta.y;
  } else if (isHandle(state.drag) && eligible && active && ImGui::IsMouseDown(state.button)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) finishPreviewInteraction(app, true);
    else editGeometry(app, view);
  }
  if (eligible) {
    drawGeometry(view, geometry(*selectedNode(app)), radialHandles(view, geometry(*selectedNode(app))),
                 isHandle(state.drag) ? state.drag : hovered ? hit : PreviewDrag::None);
    if (hovered && (isHandle(state.drag) || hit != PreviewDrag::None)) {
      const auto kind = isHandle(state.drag) ? state.drag : hit;
      ImGui::SetMouseCursor(kind == PreviewDrag::Centre ? ImGuiMouseCursor_ResizeAll :
                            kind == PreviewDrag::Left || kind == PreviewDrag::Right ? ImGuiMouseCursor_ResizeEW :
                            kind == PreviewDrag::Top || kind == PreviewDrag::Bottom ? ImGuiMouseCursor_ResizeNS :
                            ImGuiMouseCursor_Hand);
      if (state.drag == PreviewDrag::None) ImGui::SetTooltip("%s", kind == PreviewDrag::Centre ? "Move centre" :
          kind == PreviewDrag::Feather ? "Inward feather" :
          kind == PreviewDrag::Left || kind == PreviewDrag::Right ? "Horizontal radius" : "Vertical radius");
    }
  }
  return state.drag == PreviewDrag::Pan || isHandle(state.drag);
}

}  // namespace ui
