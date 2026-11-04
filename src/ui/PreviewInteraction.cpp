#include "ui/PreviewInteraction.h"

#include "AppState.h"
#include "NodeGraph.h"
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
    if (parameter.id != "centreX" && parameter.id != "centreY" && parameter.id != "radiusX" &&
        parameter.id != "radiusY" && parameter.id != "feather") continue;
    const double v = std::get<double>(parameter.value);
    if (parameter.id == "centreX") value.centreX = v;
    else if (parameter.id == "centreY") value.centreY = v;
    else if (parameter.id == "radiusX") value.radiusX = v;
    else if (parameter.id == "radiusY") value.radiusY = v;
    else if (parameter.id == "feather") value.feather = v;
  }
  return value;
}

ImVec2 mappedPoint(const PreviewTransform &view, const CoordinateMapping &mapping, double x, double y) {
  const auto p = mapping.map(x, y);
  return p ? view.toScreen(p->first, p->second) : ImVec2{};
}

// Linear displacement in screen pixels. Translation cancels; reflection,
// rotation, shear and the Preview's anisotropic scale all remain represented.
std::array<double, 2> screenVector(const PreviewTransform &view, const CoordinateMapping &mapping,
                                  double dx, double dy) {
  const auto origin = mapping.map(0, 0), end = mapping.map(dx, dy);
  if (!origin || !end) return {};
  return {(end->first - origin->first) * view.size.x,
          -(end->second - origin->second) * view.size.y};
}

ImGuiMouseCursor radiusCursor(const PreviewTransform &view, const CoordinateMapping &mapping, bool horizontal) {
  const auto axis = screenVector(view, mapping, horizontal ? 1 : 0, horizontal ? 0 : 1);
  const double angle = std::atan2(axis[1], axis[0]);
  const int direction = (int(std::lround(angle / (3.14159265358979323846 / 4))) + 4) % 4;
  const ImGuiMouseCursor cursors[] = {ImGuiMouseCursor_ResizeEW, ImGuiMouseCursor_ResizeNWSE,
                                     ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW};
  return cursors[direction];
}

void editGeometry(App &app, const PreviewTransform &view) {
  const auto &state = app.previewInteraction;
  const auto imagePoint = view.toImage(ImGui::GetIO().MousePos);
  const auto pointer = state.mapping.inverse().map(imagePoint[0], imagePoint[1]);
  if (!pointer) return;
  const double dx = pointer->first - state.startPointer[0], dy = pointer->second - state.startPointer[1];
  const auto &g = state.start;
  const auto current = geometry(*selectedNode(app));
  const auto set = [&](const char *id, double value, double previous, double lo, double hi) {
    value = std::clamp(value, lo, hi);
    if (value != previous && setNodeParameterValue(app, *state.node, id, value)) {
      app.previewInteraction.editPending = true;
      // Only our own requests may keep this gesture alive while replacement
      // pixels are pending. Worker dispatch must not look like another request.
      app.previewInteraction.editDemand = app.renderer.previewDemand();
    }
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
      const auto spoke = screenVector(view, state.mapping, g.radiusX * diagonal, g.radiusY * diagonal);
      const auto movement = screenVector(view, state.mapping, dx, dy);
      const double sx = spoke[0], sy = spoke[1];
      const double length2 = sx * sx + sy * sy;
      if (length2 > 0) {
        const double delta = (movement[0] * sx + movement[1] * sy) / length2;
        set("feather", g.feather - delta, current.feather, 0, 1);
      }
      break;
    }
    default: break;
  }
}

void drawGeometry(const PreviewTransform &view, const RadialGeometry &g, const RadialHandles &handles,
                  const CoordinateMapping &mapping, PreviewDrag highlight) {
  auto *draw = ImGui::GetWindowDrawList();
  const ImU32 black = IM_COL32(0, 0, 0, 230), white = IM_COL32(255, 255, 255, 230);
  const auto ellipse = [&](double scale, ImU32 colour) {
    const auto points = radialOutline(view, g, mapping, scale);
    draw->AddPolyline(points.data(), int(points.size()), black, ImDrawFlags_Closed, 3);
    draw->AddPolyline(points.data(), int(points.size()), colour, ImDrawFlags_Closed, 1);
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

std::vector<ImVec2> radialOutline(const PreviewTransform &view, const RadialGeometry &g,
                                 const CoordinateMapping &mapping, double scale) {
  const auto xAxis = screenVector(view, mapping, g.radiusX * scale, 0);
  const auto yAxis = screenVector(view, mapping, 0, g.radiusY * scale);
  const double extent = std::max(std::hypot(xAxis[0], xAxis[1]), std::hypot(yAxis[0], yAxis[1]));
  const int segments = int(std::clamp(extent, 48.0, 512.0));
  std::vector<ImVec2> points;
  for (int i = 0; i < segments; ++i) {
    const double angle = 6.2831853071795864769 * i / segments;
    points.push_back(mappedPoint(view, mapping, g.centreX + g.radiusX * scale * std::cos(angle),
                                g.centreY + g.radiusY * scale * std::sin(angle)));
  }
  return points;
}

RadialHandles radialHandles(const PreviewTransform &view, const RadialGeometry &g,
                            const CoordinateMapping &mapping) {
  constexpr double diagonal = 0.7071067811865475244;
  RadialHandles handles{{
      {PreviewDrag::Centre, mappedPoint(view, mapping, g.centreX, g.centreY), {}},
      {PreviewDrag::Left, mappedPoint(view, mapping, g.centreX - g.radiusX, g.centreY), {}},
      {PreviewDrag::Right, mappedPoint(view, mapping, g.centreX + g.radiusX, g.centreY), {}},
      {PreviewDrag::Top, mappedPoint(view, mapping, g.centreX, g.centreY + g.radiusY), {}},
      {PreviewDrag::Bottom, mappedPoint(view, mapping, g.centreX, g.centreY - g.radiusY), {}},
      {PreviewDrag::Feather, mappedPoint(view, mapping, g.centreX + g.radiusX * (1 - g.feather) * diagonal,
                                        g.centreY + g.radiusY * (1 - g.feather) * diagonal), {}}}};
  // Displace overlapping grips outward along their own axes/spoke, so even a
  // collapsed ellipse keeps intuitive drag directions. Centre stays anchored.
  // Leader lines retain the exact parameter positions; offsets never edit it.
  constexpr ImVec2 outward[] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1},
                               {float(diagonal), -float(diagonal)}};
  for (size_t i = 0; i < handles.size(); ++i) {
    auto &handle = handles[i];
    handle.grip = handle.anchor;
    auto direction = screenVector(view, mapping, outward[i].x / view.size.x, -outward[i].y / view.size.y);
    const double length = std::hypot(direction[0], direction[1]);
    if (length > 0) { direction[0] /= length; direction[1] /= length; }
    for (int candidate = 0; candidate < 49; ++candidate) {
      const ImVec2 point{float(handle.anchor.x + direction[0] * candidate * 18),
                         float(handle.anchor.y + direction[1] * candidate * 18)};
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

RadialPlacement radialPlacement(App &app) {
  const auto *node = selectedNode(app);
  if (!node || node->operation != NodeOperation::NativeRadialMask ||
      !dynamic_cast<const NativeRadialMaskProcessor *>(node->processor.get())) return {};
  const auto pending = [] { return RadialPlacement{PlacementState::Pending, CoordinateMapping::unknown(),
                                                  "Mask handles: Preview updating."}; };
  const auto demand = app.renderer.previewDemand();
  const auto mode = previewMode(app); // Runtime queries precede the display lock.
  const auto &gesture = app.previewInteraction;
  const bool ownEdits = isHandle(gesture.drag) && gesture.node == app.selectedNodeId() &&
                        gesture.photoPath == app.path && gesture.editDemand == demand;
  std::lock_guard<std::mutex> lock(app.displayMutex);
  if (!app.tex || app.texW <= 0 || app.texH <= 0 || app.full.px.empty()) return {};
  if (!app.textureRequest || app.textureRequest != app.processed.request ||
      app.textureGeneration != app.processed.generation ||
      (!ownEdits && app.processed.demand != demand)) return pending();
  const auto &result = app.processed.result;
  if (result.state != EvaluationState::Available) return {};
  if (mode == PreviewMode::MaskOnly) {
    const auto *mask = std::get_if<MaskValue>(&result.value);
    if (result.requested != OutputRef{node->id,ports::mask} || !mask || !*mask ||
        (*mask)->w != app.texW || (*mask)->h != app.texH) return {};
    return {PlacementState::Ready, CoordinateMapping::identity(), nullptr};
  }
  const auto *image = std::get_if<ImageValue>(&result.value);
  if (result.requested != app.documentOutput || !image || !*image ||
      (*image)->w != app.texW || (*image)->h != app.texH) return {};
  const auto placement = resolveMaskPlacement(result, {node->id,ports::mask});
  switch (placement.state) {
    case MaskPlacementState::Identity: case MaskPlacementState::Affine:
      return {PlacementState::Ready, placement.maskToImage, nullptr};
    case MaskPlacementState::Unknown:
      return {PlacementState::Unavailable, {}, "Mask handles: the image position cannot be established."};
    case MaskPlacementState::Ambiguous:
      return {PlacementState::Unavailable, {}, "Mask handles: this Mask appears in different image positions."};
    case MaskPlacementState::Absent: return {};
  }
  return {};
}

bool finishPreviewInteraction(App &app, bool force) {
  auto &state = app.previewInteraction;
  if (state.photoPath != app.path ||
      (app.isPhotoProtected() && (isHandle(state.drag) || state.editPending))) { state = {}; return false; }
  const auto *context = ImGui::GetCurrentContext();
  const auto placement = radialPlacement(app);
  const bool obsolete = isHandle(state.drag) &&
      (state.editDemand != app.renderer.previewDemand() ||
       placement.state == PlacementState::Unavailable ||
       (placement.state == PlacementState::Ready && !(placement.mapping == state.mapping)));
  if (force || obsolete || !context || ImGui::GetIO().AppFocusLost || !ImGui::IsMousePosValid() ||
      !ImGui::IsMouseDown(state.button < 0 ? 0 : state.button) ||
      context->ActiveId != state.canvas || context->ActiveIdIsAlive != state.canvas ||
      (isHandle(state.drag) && (state.node != app.selectedNodeId() ||
                               !app.renderer.canEditParameters()))) state.stop();
  if (state.drag != PreviewDrag::None || !state.editPending || !app.renderer.canCaptureSnapshot()) return false;
  state.editPending = false; // A failed capture leaves GradeHistory's pending edit intact.
  return app.history.recordCompletedEdit(app);
}

bool drawPreviewInteraction(App &app, const PreviewTransform &view, bool hovered,
                            bool active, bool textInputWasActive) {
  auto &state = app.previewInteraction;
  const auto &io = ImGui::GetIO();
  const auto placement = radialPlacement(app);
  const bool eligible = placement.state == PlacementState::Ready && app.renderer.canEditParameters();
  const auto imagePointer = view.toImage(io.MousePos);
  const auto maskPointer = eligible ? placement.mapping.inverse().map(imagePointer[0], imagePointer[1]) : std::nullopt;
  // Invert the pointer before accepting a hit/baseline, while retaining the
  // fixed screen-pixel hit radius of the projected (possibly displaced) grips.
  const auto hit = maskPointer ? hitRadialHandle(radialHandles(view, geometry(*selectedNode(app)), placement.mapping), io.MousePos) : PreviewDrag::None;
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
    // Completing an Inspector gesture can advance demand before this press.
    // Do not start a handle against that older texture in the same frame.
    const bool current = radialPlacement(app).state == PlacementState::Ready;
    state.drag = middle || space ? PreviewDrag::Pan :
        !current || hit == PreviewDrag::None ? PreviewDrag::Ignore : hit;
    state.node = app.selectedNodeId();
    state.startPointer = view.toImage(io.MousePos);
    if (isHandle(state.drag)) {
      state.start = geometry(*selectedNode(app));
      state.mapping = placement.mapping;
      state.editDemand = app.renderer.previewDemand();
      state.startPointer = {maskPointer->first, maskPointer->second};
    }
  }
  if (state.drag == PreviewDrag::Pan && ImGui::IsMouseDragging(state.button)) {
    app.previewPanX += io.MouseDelta.x; app.previewPanY += io.MouseDelta.y;
  } else if (isHandle(state.drag) && active && ImGui::IsMouseDown(state.button)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) finishPreviewInteraction(app, true);
    else if (eligible) editGeometry(app, view);
  }
  if (eligible) {
    drawGeometry(view, geometry(*selectedNode(app)), radialHandles(view, geometry(*selectedNode(app)), placement.mapping),
                 placement.mapping, isHandle(state.drag) ? state.drag : hovered ? hit : PreviewDrag::None);
    if (hovered && (isHandle(state.drag) || hit != PreviewDrag::None)) {
      const auto kind = isHandle(state.drag) ? state.drag : hit;
      ImGui::SetMouseCursor(kind == PreviewDrag::Centre ? ImGuiMouseCursor_ResizeAll :
                            kind == PreviewDrag::Left || kind == PreviewDrag::Right ? radiusCursor(view, placement.mapping, true) :
                            kind == PreviewDrag::Top || kind == PreviewDrag::Bottom ? radiusCursor(view, placement.mapping, false) :
                            ImGuiMouseCursor_Hand);
      if (state.drag == PreviewDrag::None) ImGui::SetTooltip("%s", kind == PreviewDrag::Centre ? "Move centre" :
          kind == PreviewDrag::Feather ? "Inward feather" :
          kind == PreviewDrag::Left || kind == PreviewDrag::Right ? "Horizontal radius" : "Vertical radius");
    }
  }
  if (!eligible && selectedNode(app) && selectedNode(app)->operation == NodeOperation::NativeRadialMask && placement.message) {
    const auto canvas = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText({canvas.x + 8, canvas.y + 8},
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled), placement.message);
  }
  return state.drag == PreviewDrag::Pan || isHandle(state.drag);
}

}  // namespace ui
