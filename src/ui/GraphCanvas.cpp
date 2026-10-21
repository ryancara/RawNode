#include "ui/GraphCanvas.h"

#include "AppState.h"
#include "NodeGraph.h"
#include "GraphCommands.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {
struct Canvas {
  ImVec2 origin, size;
  GraphViewport &view;
  ImDrawList &draw;

  ImVec2 screen(PersistPosition position) const {
    const auto point = graphToCanvas(view, position);
    // Keep extreme but finite sidecar positions within draw-list float range.
    return {origin.x + float(std::clamp(point.x, -1e6, 1e6)),
            origin.y + float(std::clamp(point.y, -1e6, 1e6))};
  }
  bool visible(PersistPosition position, double width, double height) const {
    const auto point = graphToCanvas(view, position);
    return point.x <= size.x && point.y <= size.y &&
           point.x + width * view.zoom >= 0 && point.y + height * view.zoom >= 0;
  }
  void text(PersistPosition position, ImU32 color, const std::string &text) const {
    draw.AddText(ImGui::GetFont(), float(ImGui::GetFontSize() * view.zoom), screen(position), color, text.c_str());
  }
};

PersistPosition portPosition(const GraphNodeLayout &node, const GraphPortLayout &port) {
  return {node.position.x + port.offset.x, node.position.y + port.offset.y};
}
ImU32 portColor(ValueType type) {
  return ImGui::GetColorU32(type == ValueType::Image ? ImGuiCol_PlotLines : ImGuiCol_PlotHistogram);
}
const GraphPortLayout *outputPort(const GraphNodeLayout &node, const PortId &id) {
  for (const auto &port : node.ports)
    if (port.declaration->direction == PortDirection::Output && port.declaration->id == id) return &port;
  return nullptr;
}
const std::optional<OutputRef> *binding(const GraphNodeLayout &node, const GraphPortLayout &port) {
  const auto &inputs = port.slot ? node.node->findSlot(*port.slot)->inputs : node.node->fixedInputs;
  const auto found = inputs.find(port.declaration->id);
  return found == inputs.end() ? nullptr : &found->second;
}
void drawConnection(const Canvas &canvas, const GraphLayout &layout, const OutputRef &producer,
                    PersistPosition consumer, ValueType type) {
  const auto found = layout.nodes.find(producer.nodeId.value);
  if (found == layout.nodes.end()) return;
  const auto *port = outputPort(found->second, producer.outputPortId);
  if (!port) return;
  const auto from = canvas.screen(portPosition(found->second, *port));
  const auto to = canvas.screen(consumer);
  const float bend = std::max(float(40 * canvas.view.zoom), std::abs(to.x - from.x) * 0.5f);
  canvas.draw.AddBezierCubic(from, {from.x + bend, from.y}, {to.x - bend, to.y}, to,
                             portColor(type), float(std::max(1.0, 2 * canvas.view.zoom)));
}
void drawConnections(const Canvas &canvas, const GraphLayout &layout, const OutputRef &output) {
  for (const auto &entry : layout.nodes) {
    const auto &node = entry.second;
    for (const auto &port : node.ports) {
      if (port.declaration->direction != PortDirection::Input) continue;
      const auto *input = binding(node, port);
      if (input && *input)
        drawConnection(canvas, layout, **input, portPosition(node, port), port.declaration->type);
    }
  }
  drawConnection(canvas, layout, output, {layout.output.x, layout.output.y + 48}, ValueType::Image);
}

void drawNode(const Canvas &canvas, const GraphNodeLayout &node, bool selected) {
  if (!canvas.visible(node.position, graphNodeWidth, node.height)) return;
  const auto low = canvas.screen(node.position);
  const auto high = canvas.screen({node.position.x + graphNodeWidth, node.position.y + node.height});
  auto &draw = canvas.draw;
  const float rounding = float(4 * canvas.view.zoom);
  draw.AddRectFilled(low, high, ImGui::GetColorU32(ImGuiCol_WindowBg), rounding);
  draw.AddRectFilled(low, canvas.screen({node.position.x + graphNodeWidth, node.position.y + graphHeaderHeight}),
                     ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : ImGuiCol_Header), rounding);
  draw.AddRect(low, high, ImGui::GetColorU32(selected ? ImGuiCol_NavCursor : ImGuiCol_Border), rounding, 0,
               selected ? 2.0f : 1.0f);
  draw.PushClipRect(low, high, true);
  const ImU32 text = ImGui::GetColorU32(node.node->enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
  canvas.text({node.position.x + 10, node.position.y + 7}, text, nodeDisplayName(*node.node));
  bool missingRequired = false;
  for (const auto &port : node.ports) {
    const auto &declaration = *port.declaration;
    const bool input = declaration.direction == PortDirection::Input;
    const auto *reference = input ? binding(node, port) : nullptr;
    const bool connected = !input || (reference && reference->has_value());
    missingRequired |= input && declaration.required && !connected;
    std::string label = declaration.type == ValueType::Image ? "Image" : "Mask";
    if (declaration.id != ports::image && declaration.id != ports::mask) label += " / " + declaration.id.value;
    if (port.slot) label += " #" + std::to_string(port.slot->value);
    const auto point = portPosition(node, port);
    const float width = ImGui::CalcTextSize(label.c_str()).x;
    canvas.text({input ? point.x + 12 : point.x - 12 - width, point.y - 8},
                 ImGui::GetColorU32(connected ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
  }
  std::string status;
  if (node.node->isMissingProcessor()) status = "Unavailable";
  if (!node.node->enabled) status += status.empty() ? "Disabled" : " / Disabled";
  if (missingRequired) status += status.empty() ? "Input required" : " / Input required";
  canvas.text({node.position.x + 10, node.position.y + node.height - 20},
               ImGui::GetColorU32(ImGuiCol_TextDisabled), status);
  draw.PopClipRect();
  // Ports straddle the border, outside the clipped node text.
  for (const auto &port : node.ports) {
    const auto &declaration = *port.declaration;
    const auto *reference = declaration.direction == PortDirection::Input ? binding(node, port) : nullptr;
    const bool connected = declaration.direction == PortDirection::Output || (reference && reference->has_value());
    const auto point = canvas.screen(portPosition(node, port));
    const auto color = portColor(declaration.type);
    const float radius = float(4 * canvas.view.zoom);
    if (connected) draw.AddCircleFilled(point, radius, color);
    else draw.AddCircle(point, radius, color, 0, declaration.required ? 2.0f : 1.0f);
  }
}
void drawOutput(const Canvas &canvas, PersistPosition position, bool selected) {
  if (!canvas.visible(position, graphOutputWidth, graphOutputHeight)) return;
  const auto low = canvas.screen(position);
  const auto high = canvas.screen({position.x + graphOutputWidth, position.y + graphOutputHeight});
  canvas.draw.AddRectFilled(low, high, ImGui::GetColorU32(ImGuiCol_WindowBg), 4);
  canvas.draw.AddRect(low, high, ImGui::GetColorU32(selected ? ImGuiCol_NavCursor : ImGuiCol_Border), 4, 0,
                        selected ? 2.0f : 1.0f);
  canvas.text({position.x + 12, position.y + 8}, ImGui::GetColorU32(ImGuiCol_Text), "Output");
  canvas.text({position.x + 12, position.y + 40}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "Image");
  canvas.draw.AddCircleFilled(canvas.screen({position.x, position.y + 48}), float(4 * canvas.view.zoom),
                              portColor(ValueType::Image));
}

const GraphNodeLayout *hitNode(const GraphLayout &layout, PersistPosition point, std::optional<NodeId> selected) {
  const auto contains = [&](const GraphNodeLayout &node) {
    return point.x >= node.position.x && point.x <= node.position.x + graphNodeWidth &&
           point.y >= node.position.y && point.y <= node.position.y + node.height;
  };
  // Selected nodes draw last and win overlapping saved rectangles.
  if (selected) {
    const auto found = layout.nodes.find(selected->value);
    if (found != layout.nodes.end() && contains(found->second)) return &found->second;
  }
  for (auto it = layout.nodes.rbegin(); it != layout.nodes.rend(); ++it)
    if (contains(it->second)) return &it->second;
  return nullptr;
}

struct GraphPortHit {
  GraphPortAddress address;
  PersistPosition position;
  ValueType type;
};

std::optional<GraphPortHit> hitGraphPort(const GraphLayout &layout, const GraphViewport &view,
                                      PersistPosition point, std::optional<NodeId> selected) {
  std::optional<GraphPortHit> hit;
  double nearest = 1;
  // A horizontal screen-space halo helps target tiny ports. Limit the vertical
  // halo to half the visible row spacing, keeping neighbouring ports distinct.
  const double radiusX = 10, radiusY = std::min(10.0, graphPortSpacing * view.zoom * 0.45);
  const auto consider = [&](GraphPortAddress address, PersistPosition position, ValueType type) {
    const auto screen = graphToCanvas(view, position);
    const double dx = (screen.x - point.x) / radiusX, dy = (screen.y - point.y) / radiusY;
    const double distance = dx * dx + dy * dy;
    if (distance <= nearest) {
      nearest = distance;
      hit = GraphPortHit{std::move(address), position, type};
    }
  };
  const auto considerNode = [&](const GraphNodeLayout &node) {
    for (const auto &port : node.ports)
      consider({node.node->id, port.slot, port.declaration->id, port.declaration->direction},
               portPosition(node, port), port.declaration->type);
  };
  // Match drawing order so equal-distance hits prefer the visually topmost port.
  const GraphNodeLayout *selectedLayout = nullptr;
  for (const auto &entry : layout.nodes) {
    if (selected == entry.second.node->id) selectedLayout = &entry.second;
    else considerNode(entry.second);
  }
  if (selectedLayout) considerNode(*selectedLayout);
  consider({{}, std::nullopt, ports::image, PortDirection::Input},
           {layout.output.x, layout.output.y + 48}, ValueType::Image);
  return hit;
}

std::optional<GraphPortHit> resolvePort(const GraphLayout &layout, const GraphPortAddress &address) {
  if (address.isDocumentOutput())
    return GraphPortHit{address, {layout.output.x, layout.output.y + 48}, ValueType::Image};
  const auto found = layout.nodes.find(address.nodeId.value);
  if (found == layout.nodes.end()) return std::nullopt;
  for (const auto &port : found->second.ports)
    if (port.slot == address.slot && port.declaration->id == address.port &&
        port.declaration->direction == address.direction)
      return GraphPortHit{address, portPosition(found->second, port), port.declaration->type};
  return std::nullopt;
}

using WireDrop = std::pair<GraphPortAddress, GraphPortAddress>;
std::optional<WireDrop> interact(App &app, const Canvas &canvas, GraphLayout &layout, bool hovered, bool active) {
  auto &state = app.graphCanvas;
  const auto &io = ImGui::GetIO();
  const PersistPosition mouse{io.MousePos.x - canvas.origin.x, io.MousePos.y - canvas.origin.y};
  std::optional<WireDrop> drop;
  const auto port = hovered ? hitGraphPort(layout, state.view, mouse, app.selectedNodeId()) : std::nullopt;
  if (state.wire && (!resolvePort(layout, *state.wire) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                    !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
                    io.KeyCtrl || io.KeySuper || io.KeyAlt)) state.wire.reset();
  if (state.wire && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (port) drop = WireDrop{*state.wire, port->address};
    state.wire.reset();
  }
  if (state.wire && (!active || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) state.wire.reset();
  if (!active || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    state.dragging.reset();
    state.panningFromEmptyCanvas = false;
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    const auto point = canvasToGraph(state.view, mouse);
    if (port) {
      state.wire = port->address;
      state.dragging.reset();
      state.panningFromEmptyCanvas = false;
    } else if (const auto *node = hitNode(layout, point, app.selectedNodeId())) {
      selectNode(app, node->node->id);
      if (!app.isPhotoProtected() && point.y < node->position.y + graphHeaderHeight) {
        state.dragging = node->node->id;
        state.dragOffset = {point.x - node->position.x, point.y - node->position.y};
      }
    } else {
      const bool overOutput = point.x >= layout.output.x && point.x <= layout.output.x + graphOutputWidth &&
                              point.y >= layout.output.y && point.y <= layout.output.y + graphOutputHeight;
      // Latch the press origin: dragging a node body into empty space must not
      // start a pan. Output is also occupied canvas, though it is not a Node.
      if (overOutput) selectOutput(app);
      else selectNode(app, std::nullopt);
      state.panningFromEmptyCanvas = !overOutput;
    }
  }
  if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                 (state.panningFromEmptyCanvas && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
    state.view.pan.x += io.MouseDelta.x; state.view.pan.y += io.MouseDelta.y;
  }
  if (hovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.MouseWheel != 0)
    zoomGraphAt(state.view, mouse, std::pow(1.15, io.MouseWheel));
  if (active && state.dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    const auto point = canvasToGraph(state.view, mouse);
    if (moveGraphNode(app, *state.dragging, {point.x - state.dragOffset.x, point.y - state.dragOffset.y}))
      layout = layoutGraph({app.nodes, app.primarySource, app.documentOutput}, app.nodePositions);
  }
  return drop;
}

void drawWireGesture(const App &app, const Canvas &canvas, const GraphLayout &layout, bool hovered) {
  if (!app.graphCanvas.wire) return;
  const auto anchor = resolvePort(layout, *app.graphCanvas.wire);
  if (!anchor) return;
  const auto &mouse = ImGui::GetIO().MousePos;
  const auto target = hovered ? hitGraphPort(layout, canvas.view,
      {mouse.x - canvas.origin.x, mouse.y - canvas.origin.y}, app.selectedNodeId()) : std::nullopt;
  const bool compatible = target && target->address.direction != anchor->address.direction &&
                          target->type == anchor->type && app.renderer.canEditParameters();
  const ImU32 color = target ? ImGui::GetColorU32(compatible ? ImGuiCol_NavCursor : ImGuiCol_PlotHistogram)
                            : portColor(anchor->type);
  const auto fixed = canvas.screen(anchor->position);
  const auto from = anchor->address.direction == PortDirection::Output ? fixed : mouse;
  const auto to = anchor->address.direction == PortDirection::Output ? mouse : fixed;
  const float bend = std::max(40.0f, std::abs(to.x - from.x) * 0.5f);
  canvas.draw.AddBezierCubic(from, {from.x + bend, from.y}, {to.x - bend, to.y}, to, color, 2);
  if (target) {
    const auto center = canvas.screen(target->position);
    canvas.draw.AddCircle(center, 8, color, 0, 2);
    if (!compatible) {
      canvas.draw.AddLine({center.x - 5, center.y - 5}, {center.x + 5, center.y + 5}, color, 2);
      canvas.draw.AddLine({center.x - 5, center.y + 5}, {center.x + 5, center.y - 5}, color, 2);
    }
    ImGui::SetTooltip("%s", !app.renderer.canEditParameters() ? "Editing unavailable: protected photograph or active export" :
        target->address.direction == anchor->address.direction ? "Connect an output to an input" :
        target->type != anchor->type ? "Image/Mask type mismatch" : "Release to connect");
  }
}

void commitWire(App &app, GraphPortAddress from, GraphPortAddress to) {
  if (!app.renderer.canEditParameters()) {
    app.setStatus("Graph editing unavailable during export or on a protected photograph");
    return;
  }
  if (from.direction == to.direction) {
    app.setStatus("Cannot connect: choose an output and an input");
    return;
  }
  if (from.direction == PortDirection::Input) std::swap(from, to);
  const OutputRef producer{from.nodeId, from.port};
  graph::CommandResult result;
  if (to.isDocumentOutput()) result = graph::setDocumentOutput(app, producer);
  else {
    const InputAddress destination{to.nodeId, to.slot, to.port};
    const auto *binding = findInput(app, destination);
    result = binding && *binding ? graph::reconnect(app, producer, destination)
                                : graph::connect(app, producer, destination);
  }
  if (!result.ok()) app.setStatus("Cannot connect: " + result.error);
}

std::optional<InputAddress> portContextMenu(App &app, const Canvas &canvas, const GraphLayout &layout,
                                            bool hovered) {
  auto &state = app.graphCanvas;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    state.wire.reset(); state.dragging.reset(); state.panningFromEmptyCanvas = false;
    const auto &mouse = ImGui::GetIO().MousePos;
    const auto port = hitGraphPort(layout, canvas.view,
        {mouse.x - canvas.origin.x, mouse.y - canvas.origin.y}, app.selectedNodeId());
    state.contextPort.reset();
    if (port && port->address.direction == PortDirection::Input && !port->address.isDocumentOutput()) {
      const auto &address = port->address;
      const auto *input = findInput(app, {address.nodeId, address.slot, address.port});
      if (input && input->has_value()) {
        selectNode(app, address.nodeId);
        state.contextPort = address;
        ImGui::OpenPopup("##portContext");
      }
    }
  }
  std::optional<InputAddress> disconnect;
  if (ImGui::BeginPopup("##portContext")) {
    if (state.contextPort && app.selectedNodeId() == state.contextPort->nodeId &&
        resolvePort(layout, *state.contextPort)) {
      const auto &port = *state.contextPort;
      const InputAddress input{port.nodeId, port.slot, port.port};
      const auto *binding = findInput(app, input);
      if (ImGui::MenuItem("Disconnect", nullptr, false,
                          binding && binding->has_value() && app.renderer.canEditParameters())) disconnect = input;
    } else ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return disconnect;
}

std::optional<NodeId> nodeContextMenu(App &app, const Canvas &canvas, const GraphLayout &layout, bool hovered) {
  auto &state = app.graphCanvas;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const auto &mouse = ImGui::GetIO().MousePos;
    const PersistPosition point{mouse.x - canvas.origin.x, mouse.y - canvas.origin.y};
    // Ports own right-clicks too, including ports without a menu operation.
    if (!hitGraphPort(layout, canvas.view, point, app.selectedNodeId())) {
      const auto *node = hitNode(layout, canvasToGraph(canvas.view, point), app.selectedNodeId());
      state.contextNode = node ? std::optional<NodeId>{node->node->id} : std::nullopt;
      if (node) {
        selectNode(app, node->node->id);
        if (node->node->operation != NodeOperation::Source) ImGui::OpenPopup("##nodeContext");
      }
    }
  }
  std::optional<NodeId> remove;
  if (ImGui::BeginPopup("##nodeContext")) {
    const Node *node = state.contextNode ? findNode(app, *state.contextNode) : nullptr;
    if (!node || app.selectedNodeId() != state.contextNode) ImGui::CloseCurrentPopup();
    else if (ImGui::MenuItem("Delete Node", nullptr, false,
                             node->operation != NodeOperation::Source && app.renderer.canEditParameters()))
      remove = node->id;
    ImGui::EndPopup();
  }
  return remove;
}

}  // namespace

void drawGraphCanvas(App &app) {
  auto &state = app.graphCanvas;
  if (state.photoPath != app.path) {
    state = GraphCanvasState{};
    state.photoPath = app.path;
  }
  if (ImGui::Button("Frame all (F)")) state.framePending = true;
  ImGui::SameLine();
  ImGui::TextDisabled("Drag headers / Drag empty canvas or middle-drag to pan / Wheel zoom");
  const auto size = ImGui::GetContentRegionAvail();
  if (size.x < 1 || size.y < 1) { state.cancelInteraction(); return; }
  if (!ImGui::BeginChild("##graphCanvasRegion", size, ImGuiChildFlags_None,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    state.cancelInteraction(); ImGui::EndChild(); return;
  }
  const auto canvasOrigin = ImGui::GetCursorScreenPos(), canvasSize = ImGui::GetContentRegionAvail();
  if (canvasSize.x < 1 || canvasSize.y < 1) { state.cancelInteraction(); ImGui::EndChild(); return; }
  ImGui::InvisibleButton("##graphCanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft |
                          ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_EnableNav);
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  const auto &io = ImGui::GetIO();
  if (ImGui::IsItemFocused() && !io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt &&
      ImGui::IsKeyPressed(ImGuiKey_F, false)) state.framePending = true;
  std::optional<NodeId> remove;
  if (ImGui::IsItemFocused() && ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() &&
      !io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift &&
      app.renderer.canEditParameters() && selectedNode(app) &&
      (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
    remove = app.selectedNodeId();
  auto layout = layoutGraph({app.nodes, app.primarySource, app.documentOutput}, app.nodePositions);
  // Materialize only missing auto positions, in the existing grade layout map.
  // No disk writes: ordinary Save Grade/photo departure/shutdown captures these.
  if (!app.isPhotoProtected())
    for (const auto &entry : layout.nodes) app.nodePositions.emplace(entry.first, entry.second.position);
  // Newly docked windows can report their pre-dock size on the first frame.
  // Fit once geometry settles; later user resizing does not erase their pan.
  const bool sizeStable = state.lastCanvasSize.x == canvasSize.x && state.lastCanvasSize.y == canvasSize.y;
  state.lastCanvasSize = {canvasSize.x, canvasSize.y};
  if (state.framePending && sizeStable) {
    frameGraph(state.view, layout, {canvasSize.x, canvasSize.y});
    state.framePending = false;
  }
  Canvas canvas{canvasOrigin, canvasSize, state.view, *ImGui::GetWindowDrawList()};
  const auto drop = interact(app, canvas, layout, hovered, active);
  const ImVec2 high(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y);
  canvas.draw.PushClipRect(canvasOrigin, high, true);
  canvas.draw.AddRectFilled(canvasOrigin, high, ImGui::GetColorU32(ImGuiCol_FrameBg));
  drawConnections(canvas, layout, app.documentOutput);
  const GraphNodeLayout *selected = nullptr;
  for (const auto &entry : layout.nodes) {
    if (app.selectedNodeId() == entry.second.node->id) selected = &entry.second;
    else drawNode(canvas, entry.second, false);
  }
  if (selected) drawNode(canvas, *selected, true);
  drawOutput(canvas, layout.output, app.outputSelected());
  drawWireGesture(app, canvas, layout, hovered);
  canvas.draw.PopClipRect();
  const auto disconnect = portContextMenu(app, canvas, layout, hovered);
  if (const auto menuRemove = nodeContextMenu(app, canvas, layout, hovered)) remove = menuRemove;
  ImGui::EndChild();
  // All frame-local geometry borrows live nodes. Dispatch after drawing/menus.
  if (remove && app.renderer.canEditParameters()) destroyNode(app, *remove);
  else if (drop) commitWire(app, drop->first, drop->second);
  else if (disconnect && app.renderer.canEditParameters()) {
    const auto result = graph::disconnect(app, *disconnect);
    if (!result.ok()) app.setStatus("Cannot disconnect: " + result.error);
  }
}
}  // namespace ui
