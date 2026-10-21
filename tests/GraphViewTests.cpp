#include "GraphViewTests.h"

#include "AppState.h"
#include "GraphCommands.h"
#include "GraphValidation.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/DockLayout.h"
#include "ui/GraphLayout.h"
#include "ui/UiContext.h"

#include "imgui.h"
#include "imgui_internal.h"
#include <tiffio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "graph view check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
GraphView graph(const App &app) { return {app.nodes, app.primarySource, app.documentOutput}; }
bool same(PersistPosition a, PersistPosition b) { return std::abs(a.x - b.x) < 1e-8 && std::abs(a.y - b.y) < 1e-8; }
bool samePositions(const PersistPositions &a, const PersistPositions &b) {
  if (a.size() != b.size()) return false;
  for (const auto &entry : a) {
    const auto found = b.find(entry.first);
    if (found == b.end() || !same(entry.second, found->second)) return false;
  }
  return true;
}
bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto state) {
    return !state.previewPending && !state.previewBusy && !state.displayPending;
  });
}

bool dependencyLayoutAndPorts() {
  App app;
  // Hand-authored diamond, optional Mask fan-out, and a disconnected required
  // Image input. Display expectations come from these bindings, never storage.
  Node left(NodeOperation::TestMultipleOutputs); left.id = NodeId{"left"};
  left.fixedInputs.at(ports::image) = OutputRef{app.primarySource, ports::image};
  Node right(NodeOperation::TestImage); right.id = NodeId{"right"}; right.enabled = false;
  right.fixedInputs.at(ports::image) = OutputRef{app.primarySource, ports::image};
  Node merge(NodeOperation::TestSlotted); merge.id = NodeId{"merge"};
  auto slot = merge.makeSlot();
  slot.inputs.at(ports::image) = OutputRef{left.id, ports::image};
  slot.inputs.at(ports::mask) = OutputRef{left.id, ports::mask};
  merge.slots.push_back(slot);
  slot = merge.makeSlot(); slot.inputs.at(ports::image) = OutputRef{right.id, ports::image};
  merge.slots.push_back(slot);
  Node spare; spare.id = NodeId{"spare"}; spare.storedLabel = "Unavailable";
  app.documentOutput = {merge.id, ports::image};
  app.nodes.push_back(std::move(merge)); app.nodes.push_back(std::move(spare));
  app.nodes.push_back(std::move(right)); app.nodes.push_back(std::move(left));
  const auto layout = ui::layoutGraph(graph(app), {});
  const auto &root = layout.nodes.at("source"), &a = layout.nodes.at("left"), &b = layout.nodes.at("right");
  const auto &end = layout.nodes.at("merge"), &disconnected = layout.nodes.at("spare");
  CHECK(layout.nodes.size() == 5 && root.position.x < a.position.x && root.position.x < b.position.x);
  CHECK(a.position.x == b.position.x && a.position.y + a.height < b.position.y);
  CHECK(end.position.x > a.position.x && end.position.x > b.position.x && layout.output.x > end.position.x);
  CHECK(disconnected.node->isMissingProcessor() && !b.node->enabled);
  CHECK(!disconnected.node->fixedInputs.at(ports::image));
  CHECK(end.ports.size() == 6 && a.ports.size() == 4 && root.ports.size() == 1);
  CHECK(a.ports[3].declaration->type == ValueType::Mask && a.ports[3].declaration->direction == PortDirection::Output);
  CHECK(end.ports[2].slot == SlotId{1} && end.ports[2].declaration->required);
  CHECK(end.ports[3].declaration->type == ValueType::Mask && !end.ports[3].declaration->required);
  CHECK(end.ports[4].slot == SlotId{2});
  // Positions and port geometry must be identical after complete storage reversal.
  std::reverse(app.nodes.begin(), app.nodes.end());
  const auto reversed = ui::layoutGraph(graph(app), {});
  for (const auto &entry : layout.nodes) {
    const auto &other = reversed.nodes.at(entry.first);
    CHECK(same(entry.second.position, other.position) && entry.second.height == other.height);
    CHECK(entry.second.ports.size() == other.ports.size());
    for (size_t i = 0; i < other.ports.size(); ++i) {
      CHECK(entry.second.ports[i].declaration == other.ports[i].declaration);
      CHECK(entry.second.ports[i].slot == other.ports[i].slot);
      CHECK(same(entry.second.ports[i].offset, other.ports[i].offset));
    }
  }
  // Preserve fractional/negative coordinates; avoid a saved node occupying the
  // default first-column slot. Stale IDs cannot create visual/executable nodes.
  const PersistPositions saved{{"source", {-15.5, 8.25}}, {"spare", {320, 0}}, {"stale", {0, 0}}};
  const auto restored = ui::layoutGraph(graph(app), saved);
  CHECK(same(restored.nodes.at("source").position, saved.at("source")));
  CHECK(same(restored.nodes.at("spare").position, saved.at("spare")));
  CHECK(restored.nodes.at("left").position.y >= disconnected.height + 40);
  CHECK(restored.nodes.size() == 5 && app.nodePositions.empty());
  return true;
}

bool coordinateTransforms() {
  ui::GraphViewport view{{35, -70}, 0.75};
  const PersistPosition point{-24.5, 132.25}, cursor{213, 127};
  CHECK(same(ui::canvasToGraph(view, ui::graphToCanvas(view, point)), point));
  const auto anchor = ui::canvasToGraph(view, cursor);
  ui::zoomGraphAt(view, cursor, 1.5);
  CHECK(view.zoom == 1.125 && same(ui::canvasToGraph(view, cursor), anchor));
  ui::zoomGraphAt(view, cursor, 1e6);
  CHECK(view.zoom == ui::graphMaxZoom && same(ui::canvasToGraph(view, cursor), anchor));
  ui::zoomGraphAt(view, cursor, 1e-9);
  CHECK(view.zoom == ui::graphMinZoom && same(ui::canvasToGraph(view, cursor), anchor));
  const auto previous = view;
  ui::zoomGraphAt(view, cursor, std::numeric_limits<double>::quiet_NaN());
  CHECK(view.zoom == previous.zoom && same(view.pan, previous.pan));
  App app;
  const auto layout = ui::layoutGraph(graph(app), {{"source", {-100, -200}}});
  ui::frameGraph(view, layout, {800, 300});
  // Source [-100,-200] to Output [360,-120]: width 460, height 80. At zoom 1
  // frame-all centers this rectangle at (400,150), independently calculated.
  CHECK(view.zoom == 1 && same(view.pan, {270, 310}));
  for (const auto &entry : layout.nodes) {
    const auto low = ui::graphToCanvas(view, entry.second.position);
    const auto high = ui::graphToCanvas(view, {entry.second.position.x + ui::graphNodeWidth,
                                              entry.second.position.y + entry.second.height});
    CHECK(low.x >= 32 && low.y >= 32 && high.x <= 768 && high.y <= 268);
  }
  return true;
}

// Real ImGui frames/input; no GLFW window, GL calls, or test-only input handler.
struct Ui {
  ImGuiContext *context = ImGui::CreateContext();
  ImVec2 listButton{}, graphButton{}, frameButton{}, canvasOrigin{}, canvasSize{};
  bool showOtherPanel = false, showInspector = false;
  ImGuiID inspectorLastItem = 0;
  ImGuiItemFlags inspectorItemFlags = 0;
  ImRect inspectorLastRect;
  ImVec2 listSource{}, listOutput{}, addButton{};
  float listRight = 0;
  float nodesWidth = 1150;
  bool pickerOpen = false, captureLog = false;
  std::string nodesLog, inspectorLog;
  ImGuiID inspectorId(const char *label) const {
    return ImGui::FindWindowByName("Inspector test")->GetID(label);
  }
  char text[32] = {};
  ImVec2 textInput{}, otherButton{};
  Ui() {
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char *pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }
  ~Ui() { ImGui::DestroyContext(context); }
  void tick(App &app) {
    auto &io = ImGui::GetIO(); io.DisplaySize = {1280, 800}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({20, 20});
    ImGui::SetNextWindowSize({(showOtherPanel || showInspector) ? 900.0f : nodesWidth, 680});
    ImGui::Begin("Nodes test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto start = ImGui::GetCursorScreenPos(); const auto &style = ImGui::GetStyle();
    listButton = {start.x + 8, start.y + 8};
    graphButton = {start.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("List").x +
                     2 * style.ItemSpacing.x + ImGui::CalcTextSize("|").x + 8, start.y + 8};
    frameButton = {start.x + 20, start.y + ImGui::GetFrameHeight() + style.ItemSpacing.y +
                   style.ItemSpacing.y + 1 + 8};
    if (captureLog) ImGui::LogToBuffer(0);
    drawNodesPanel(app);
    if (captureLog) { nodesLog = ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish(); }
    addButton = {graphButton.x + 95, start.y + 8};
    pickerOpen = ImGui::IsPopupOpen("##addPluginPopup");
    ImGui::End();
    for (auto *window : ImGui::GetCurrentContext()->Windows) {
      if (std::string(window->Name).find("Nodes test/nodeList") == std::string::npos) continue;
      listSource = {window->DC.CursorStartPos.x + 30, window->DC.CursorStartPos.y + 6};
      // Output is the final fixed row, immediately before EndChild.
      listOutput = {window->DC.CursorPosPrevLine.x + 30, window->DC.CursorPosPrevLine.y + 6};
      listRight = window->WorkRect.Max.x;
    }
    if (showInspector) {
      ImGui::SetNextWindowPos({940, 20}); ImGui::SetNextWindowSize({320, 680});
      ImGui::Begin("Inspector test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoResize);
      if (captureLog) ImGui::LogToBuffer(0);
      drawInspector(app);
      if (captureLog) { inspectorLog = ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish(); }
      inspectorLastItem = ImGui::GetItemID();
      inspectorItemFlags = ImGui::GetCurrentContext()->LastItemData.ItemFlags;
      inspectorLastRect = ImGui::GetCurrentContext()->LastItemData.Rect;
      ImGui::End();
    }
    if (showOtherPanel) {
      ImGui::SetNextWindowPos({940, 20}); ImGui::SetNextWindowSize({300, 180});
      ImGui::Begin("Other panel", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoResize);
      ImGui::InputText("Text", text, sizeof text);
      textInput = {ImGui::GetItemRectMin().x + 20, ImGui::GetItemRectMin().y + 8};
      ImGui::Button("Focus other panel");
      otherButton = {ImGui::GetItemRectMin().x + 20, ImGui::GetItemRectMin().y + 8};
      ImGui::End();
    }
    ImGui::EndFrame();
    for (auto *window : ImGui::GetCurrentContext()->Windows) {
      if (std::string(window->Name).find("Nodes test/##graphCanvasRegion") == std::string::npos) continue;
      canvasOrigin = window->DC.CursorStartPos;
      canvasSize = {window->WorkRect.GetWidth(), window->WorkRect.GetHeight()};
    }
  }
  void hover(App &app, ImVec2 point) { ImGui::GetIO().AddMousePosEvent(point.x, point.y); tick(app); }
  void click(App &app, ImVec2 point) {
    hover(app, point); ImGui::GetIO().AddMouseButtonEvent(0, true); tick(app);
    ImGui::GetIO().AddMouseButtonEvent(0, false); tick(app);
  }
  void drag(App &app, ImVec2 from, ImVec2 delta, int button) {
    hover(app, from); ImGui::GetIO().AddMouseButtonEvent(button, true); tick(app);
    hover(app, {from.x + delta.x, from.y + delta.y});
    ImGui::GetIO().AddMouseButtonEvent(button, false); tick(app);
  }
  void pressF(App &app, bool enterText = false) {
    ImGui::GetIO().AddKeyEvent(ImGuiKey_F, true);
    if (enterText) ImGui::GetIO().AddInputCharacter('f');
    tick(app);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_F, false); tick(app);
  }
  ImVec2 listProcessorFromEnd(int offset = 0, bool removeButton = false) const {
    // Processor rows end immediately above the separator and fixed Output row.
    return {removeButton ? listRight - ImGui::GetFrameHeight() / 2 : listOutput.x,
            listOutput.y - (offset + 1) * ImGui::GetFrameHeightWithSpacing() -
              1 - 2 * ImGui::GetStyle().ItemSpacing.y};
  }
  bool listFocused() const {
    return context->NavWindow && std::string(context->NavWindow->Name).find("Nodes test/nodeList") == 0;
  }
  ImVec2 screen(const App &app, PersistPosition point) const {
    const auto canvas = ui::graphToCanvas(app.graphCanvas.view, point);
    return {canvasOrigin.x + float(canvas.x), canvasOrigin.y + float(canvas.y)};
  }
};

std::string graphJson(const App &app) {
  PersistFile file; file.grade.emplace(); file.grade->graph = captureGraph(graph(app));
  file.grade->color = captureGradeColor(app);
  std::string json, error;
  if (!encodeGraphFile(file, json, error)) throw std::runtime_error(error);
  return json;
}
void observeMutation(void *context, RenderRuntimeTestAccess::Event event) noexcept {
  if (event == RenderRuntimeTestAccess::Event::MutationCompleted)
    ++*static_cast<std::atomic<int> *>(context);
}

bool canvasInteraction() {
  Ui ui;
  App app;
  app.full.w = app.full.h = 1; app.full.px = {0.25f, 0.25f, 0.25f, 1};
  app.preview = std::make_shared<Image>(app.full); app.path = "canvas-a";
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId();
  app.renderer.start(); CHECK(idle(app));
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  const auto topology = graphJson(app);
  std::atomic<int> mutations{0};
  RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
  ui.tick(app); ui.tick(app);
  ui.click(app, ui.graphButton);
  CHECK(app.nodesView == NodesView::Graph && app.selectedNodeId() == b);
  CHECK(app.nodePositions.size() == 3); // Source and both processors, no virtual Output.
  app.graphCanvas.view = {{30, 30}, 1}; app.graphCanvas.framePending = false; ui.tick(app);
  const auto original = app.nodePositions.at(a.value);
  app.paramFilter[0] = 'x';
  ui.click(app, ui.screen(app, {original.x + 70, original.y + 16}));
  CHECK(app.selectedNodeId() == a && app.paramFilter[0] == '\0');
  ui.drag(app, ui.screen(app, {original.x + 70, original.y + 16}), {45, 55}, 0);
  CHECK(same(app.nodePositions.at(a.value), {original.x + 45, original.y + 55}));
  const auto moved = app.nodePositions.at(a.value);
  // Body clicks select, but cannot reposition. Source headers can reposition.
  const auto bodyPan = app.graphCanvas.view.pan;
  ui.drag(app, ui.screen(app, {moved.x + 70, moved.y + 45}), {20, 25}, 0);
  CHECK(same(app.nodePositions.at(a.value), moved) && same(app.graphCanvas.view.pan, bodyPan));
  const auto source = app.nodePositions.at(app.primarySource.value);
  ui.click(app, ui.screen(app, {source.x + 50, source.y + 45}));
  CHECK(app.selectedNodeId() == app.primarySource && !selectedNode(app) && same(app.nodePositions.at(app.primarySource.value), source));
  CHECK(same(app.graphCanvas.view.pan, bodyPan));
  ui.click(app, ui.screen(app, {moved.x + 70, moved.y + 45})); CHECK(app.selectedNodeId() == a);
  ui.drag(app, ui.screen(app, {source.x + 50, source.y + 16}), {12, 18}, 0);
  CHECK(same(app.nodePositions.at(app.primarySource.value), {source.x + 12, source.y + 18}));
  CHECK(app.selectedNodeId() == app.primarySource && app.paramFilter[0] == '\0');
  ui.click(app, ui.listButton); CHECK(app.selectedNodeId() == app.primarySource);
  ui.click(app, ui.graphButton); CHECK(app.selectedNodeId() == app.primarySource);
  ui.click(app, ui.screen(app, {moved.x + 70, moved.y + 45}));
  CHECK(app.selectedNodeId() == a);
  const auto positions = app.nodePositions;
  const auto leftPan = app.graphCanvas.view.pan;
  const ImVec2 blank{ui.canvasOrigin.x + 600, ui.canvasOrigin.y + 300};
  // Start empty, then cross over a header. Pan depends on the press origin.
  ui.drag(app, blank, {-200, -200}, 0);
  CHECK(same(app.graphCanvas.view.pan, {leftPan.x - 200, leftPan.y - 200}));
  CHECK(samePositions(app.nodePositions, positions) && !app.selectedNodeId());
  const auto pan = app.graphCanvas.view.pan;
  ui.drag(app, blank, {40, -20}, 2);
  CHECK(same(app.graphCanvas.view.pan, {pan.x + 40, pan.y - 20}));
  ui.hover(app, blank);
  const PersistPosition cursor{blank.x - ui.canvasOrigin.x, blank.y - ui.canvasOrigin.y};
  const auto anchor = ui::canvasToGraph(app.graphCanvas.view, cursor);
  ImGui::GetIO().AddMouseWheelEvent(0, 1); ui.tick(app);
  CHECK(app.graphCanvas.view.zoom > 1 && same(ui::canvasToGraph(app.graphCanvas.view, cursor), anchor));
  ui.click(app, blank); // Give the canvas keyboard focus.
  const auto unframed = app.graphCanvas.view.pan;
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F, true); ui.tick(app);
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F, false); ui.tick(app);
  CHECK(!same(app.graphCanvas.view.pan, unframed));
  CHECK(samePositions(app.nodePositions, positions));
  const auto framedPan = app.graphCanvas.view.pan;
  const auto output = ui::layoutGraph(graph(app), app.nodePositions).output;
  ui.drag(app, ui.screen(app, {output.x + 30, output.y + 40}), {20, 25}, 0);
  CHECK(same(app.graphCanvas.view.pan, framedPan) && samePositions(app.nodePositions, positions));
  app.paramFilter[0] = 'e';
  ui.click(app, ui.listButton);
  CHECK(app.nodesView == NodesView::List && app.outputSelected() && app.paramFilter[0] == 'e');
  const auto listPan = app.graphCanvas.view.pan;
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F, true); ui.tick(app);
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F, false); ui.tick(app);
  CHECK(same(app.graphCanvas.view.pan, listPan));
  ui.click(app, ui.graphButton);
  CHECK(app.outputSelected() && app.paramFilter[0] == 'e' && same(app.nodePositions.at(a.value), moved));
  // Toolbar framing works after a viewport change, without moving document data.
  app.graphCanvas.view.pan = {900, 900}; ui.tick(app);
  ui.click(app, ui.frameButton);
  CHECK(!same(app.graphCanvas.view.pan, {900, 900}));
  // Protected photos still allow viewing, selection and navigation, not layout edits.
  app.sidecarWriteBlockedPath = app.path;
  CHECK(!ui::moveGraphNode(app, a, {100, 100}));
  ui.drag(app, ui.screen(app, {moved.x + 70, moved.y + 16}), {50, 50}, 0);
  CHECK(same(app.nodePositions.at(a.value), moved));
  const auto protectedPan = app.graphCanvas.view.pan;
  const ImVec2 protectedBlank{ui.canvasOrigin.x + 20, ui.canvasOrigin.y + 20};
  ui.drag(app, protectedBlank, {20, 30}, 0);
  CHECK(same(app.graphCanvas.view.pan, {protectedPan.x + 20, protectedPan.y + 30}));
  CHECK(samePositions(app.nodePositions, positions));
  CHECK(!ui::moveGraphNode(app, NodeId{"missing"}, {0, 0}));
  app.sidecarWriteBlockedPath.clear();
  CHECK(!ui::moveGraphNode(app, a, {std::numeric_limits<double>::infinity(), 0}));
  CHECK(topology == graphJson(app) && mutations == 0);
  const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(before.epoch == after.epoch && before.mutationDepth == after.mutationDepth);
  CHECK(!after.previewPending && !after.quietPending && !after.displayPending && !after.previewBusy && !after.exportBusy);
  CHECK(app.previewZoom == 1 && app.previewPanX == 0 && app.previewPanY == 0);
  // Visual edits also preserve existing queued work while export owns execution.
  {
    auto exportOwner = app.renderer.acquireExport(); CHECK(exportOwner);
    app.renderer.requestPreview(); app.renderer.requestDisplayRefresh();
    const auto queued = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(queued.previewPending && queued.displayPending && queued.exportBusy);
    CHECK(ui::moveGraphNode(app, a, {-12, 18}));
    ui.click(app, ui.listButton); ui.click(app, ui.graphButton);
    const auto stillQueued = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(stillQueued.epoch == queued.epoch && stillQueued.previewPending && stillQueued.displayPending);
    CHECK(stillQueued.exportBusy && mutations == 0 && graphJson(app) == topology);
  }
  CHECK(idle(app));
  app.renderer.shutdown();
  RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  return true;
}

bool zoomedNodeInteraction() {
  for (const double zoom : {0.5, 2.0}) {
    Ui ui; App app; app.nodesView = NodesView::Graph;
    CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
    CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId();
    ui.tick(app); ui.tick(app);
    app.nodePositions = {{app.primarySource.value, {-300, 150}}, {a.value, {60, 40}}, {b.value, {500, 200}}};
    app.graphCanvas.view = {{25, 25}, zoom}; app.graphCanvas.framePending = false;
    ui.tick(app);
    const auto topology = graphJson(app);
    const auto positions = app.nodePositions;
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    const auto pan = app.graphCanvas.view.pan;
    // Screen coordinates are calculated independently of graphToCanvas().
    const ImVec2 header{ui.canvasOrigin.x + float(25 + (60 + 70) * zoom),
                        ui.canvasOrigin.y + float(25 + (40 + 16) * zoom)};
    ui.click(app, header); CHECK(app.selectedNodeId() == a);
    ui.drag(app, header, {60, 30}, 0);
    const PersistPosition moved{60 + 60 / zoom, 40 + 30 / zoom};
    CHECK(same(app.nodePositions.at(a.value), moved) && same(app.graphCanvas.view.pan, pan));
    CHECK(same(app.nodePositions.at(b.value), positions.at(b.value)));
    CHECK(same(app.nodePositions.at(app.primarySource.value), positions.at(app.primarySource.value)));
    const auto afterDrag = app.nodePositions;
    const ImVec2 body{ui.canvasOrigin.x + float(25 + (moved.x + 70) * zoom),
                      ui.canvasOrigin.y + float(25 + (moved.y + 45) * zoom)};
    // A body-origin drag must remain inert even after crossing into empty space.
    ui.drag(app, body, {400, 280}, 0);
    CHECK(samePositions(app.nodePositions, afterDrag) && same(app.graphCanvas.view.pan, pan));
    const PersistPosition cursor{380, 260};
    ui.hover(app, {ui.canvasOrigin.x + 380, ui.canvasOrigin.y + 260});
    const PersistPosition anchor{(cursor.x - pan.x) / zoom, (cursor.y - pan.y) / zoom};
    ImGui::GetIO().AddMouseWheelEvent(0, 1); ui.tick(app);
    CHECK(std::abs(app.graphCanvas.view.zoom - zoom * 1.15) < 1e-8);
    const auto &view = app.graphCanvas.view;
    CHECK(same({anchor.x * view.zoom + view.pan.x, anchor.y * view.zoom + view.pan.y}, cursor));
    CHECK(samePositions(app.nodePositions, afterDrag) && graphJson(app) == topology);
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(before.epoch == after.epoch && before.previewPending == after.previewPending);
    CHECK(before.displayPending == after.displayPending && before.mutationDepth == after.mutationDepth);
  }
  return true;
}

bool frameShortcutFocus() {
  Ui ui; App app; app.nodesView = NodesView::Graph; ui.showOtherPanel = true;
  CHECK(addNativeExposureNode(app)); const auto id = *app.selectedNodeId();
  ui.tick(app); ui.tick(app);
  app.nodePositions = {{app.primarySource.value, {0, 0}}, {id.value, {300, 90}}};
  app.graphCanvas.view = {{5, 15}, 0.5}; app.graphCanvas.framePending = false;
  ui.tick(app);
  const auto positions = app.nodePositions;
  const auto topology = graphJson(app);
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  const ImVec2 blank{ui.canvasOrigin.x + 400, ui.canvasOrigin.y + 300};
  ui.click(app, blank); ui.pressF(app);
  // Known world bounds: Source (0,0), processor (300,90), Output ending
  // at (760,170). This canvas fits them at 1x, centered about (380,85).
  CHECK(app.graphCanvas.view.zoom == 1);
  CHECK(same(app.graphCanvas.view.pan, {ui.canvasSize.x / 2 - 380, ui.canvasSize.y / 2 - 85}));
  app.graphCanvas.view = {{-140, 75}, 0.75}; ui.tick(app);
  const auto unframed = app.graphCanvas.view;
  ui.click(app, ui.textInput); ui.hover(app, blank);
  CHECK(ImGui::GetIO().WantTextInput);
  ui.pressF(app, true);
  CHECK(std::string(ui.text) == "f");
  CHECK(same(app.graphCanvas.view.pan, unframed.pan) && app.graphCanvas.view.zoom == unframed.zoom);
  ui.click(app, ui.otherButton); ui.hover(app, blank);
  CHECK(!ImGui::GetIO().WantTextInput);
  ui.pressF(app);
  CHECK(same(app.graphCanvas.view.pan, unframed.pan) && app.graphCanvas.view.zoom == unframed.zoom);
  ui.click(app, ui.listButton); ui.pressF(app);
  CHECK(app.nodesView == NodesView::List && same(app.graphCanvas.view.pan, unframed.pan));
  ui.click(app, ui.graphButton); ui.pressF(app);
  CHECK(app.nodesView == NodesView::Graph && same(app.graphCanvas.view.pan, unframed.pan));
  ui.click(app, blank); ui.pressF(app);
  CHECK(app.graphCanvas.view.zoom == 1);
  CHECK(same(app.graphCanvas.view.pan, {ui.canvasSize.x / 2 - 380, ui.canvasSize.y / 2 - 85}));
  CHECK(samePositions(app.nodePositions, positions) && graphJson(app) == topology);
  const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(before.epoch == after.epoch && before.previewPending == after.previewPending);
  return true;
}

bool syntheticPhoto(const std::string &, Image &image, ColorEncoding &encoding, bool &raw, ColorEncoding) {
  image.w = image.h = 1; image.px = {0.25f, 0.25f, 0.25f, 1};
  encoding = {RgbGamut::Rec709, TransferFunction::SRGB}; raw = false; return true;
}
std::string bytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}};
}
struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
      ("rawnode-graph-view-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { std::filesystem::create_directory(dir); }
  ~Files() { std::error_code ignored; std::filesystem::remove_all(dir, ignored); }
};

bool photoLayoutLifecycle() {
  Files files;
  const auto a = (files.dir / "a.png").string(), b = (files.dir / "b.png").string();
  std::ofstream(a) << "synthetic"; std::ofstream(b) << "synthetic";
  document_detail::ActionTestOptions options; options.decode = syntheticPhoto;
  App app;
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(addNativeExposureNode(app)); const auto id = *app.selectedNodeId();
  Ui ui; app.nodesView = NodesView::Graph;
  const auto executable = graphJson(app);
  const auto beforeLayout = RenderRuntimeTestAccess::snapshot(app.renderer);
  ui.tick(app); ui.tick(app);
  // The accepted contract persists every initial position, including nodes that
  // have never been dragged. Source and its one consumer start at (0,0)/(320,0).
  const auto generated = app.nodePositions;
  CHECK(generated.size() == 2 && same(generated.at(app.primarySource.value), {0, 0}));
  CHECK(same(generated.at(id.value), {320, 0}) && graphJson(app) == executable);
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == beforeLayout.epoch);
  selectOutput(app);
  CHECK(document_detail::openPhoto(app, b, {}, options));
  CHECK(!app.outputSelected() && !app.selectedNodeId());
  CHECK(selectNode(app, app.primarySource));
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(!app.outputSelected() && !app.selectedNodeId()); ui.tick(app); ui.tick(app);
  CHECK(samePositions(app.nodePositions, generated));
  CHECK(ui::moveGraphNode(app, id, {-42.5, 19.25}));
  // Leave Source at its generated position while manually moving the processor.
  const auto originalFile = bytes(inputSidecarPath(a));
  CHECK(ui::moveGraphNode(app, id, {-64.5, 80.125}));
  CHECK(bytes(inputSidecarPath(a)) == originalFile); // movement performs no I/O.
  options.beforeSave = [] { return persist_detail::FileFault::Replace; };
  CHECK(!document_detail::openPhoto(app, b, {}, options));
  CHECK(app.path == a && same(app.nodePositions.at(id.value), {-64.5, 80.125}));
  CHECK(bytes(inputSidecarPath(a)) == originalFile && !app.persistenceWarning.empty());
  options.beforeSave = {};
  CHECK(document_detail::openPhoto(app, b, {}, options) && app.nodePositions.empty());
  PersistFile saved; std::string error;
  CHECK(readGraphSidecar(inputSidecarPath(a), saved, error) == SidecarReadStatus::Loaded && saved.grade);
  CHECK(saved.grade->positions.size() == 2 && same(saved.grade->positions.at(id.value), {-64.5, 80.125}));
  CHECK(same(saved.grade->positions.at(app.primarySource.value), generated.at(app.primarySource.value)));
  CHECK(saved.grade->graph.nodes.size() == 2 && saved.grade->graph.documentOutput.nodeId == id);
  // The normal codec and reopen path preserve exactly the stable-ID layout.
  std::string json; PersistFile decoded;
  CHECK(encodeGraphFile(saved, json, error) && decodeGraphFile(json, decoded, error));
  CHECK(same(decoded.grade->positions.at(id.value), {-64.5, 80.125}));
  CHECK(json.find("zoom") == std::string::npos && json.find("pan") == std::string::npos);
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(findNode(app, id) && same(app.nodePositions.at(id.value), {-64.5, 80.125}));
  ui.tick(app); ui.tick(app);
  CHECK(same(app.nodePositions.at(app.primarySource.value), generated.at(app.primarySource.value)));
  app.graphCanvas.view.pan = {999, 999};
  CHECK(document_detail::openPhoto(app, b, {}, options)); ui.tick(app);
  CHECK(app.graphCanvas.photoPath == b && !same(app.graphCanvas.view.pan, {999, 999}));
  CHECK(!app.selectedNodeId() && app.nodesView == NodesView::Graph && app.nodePositions.size() == 1);
  CHECK(ui::moveGraphNode(app, app.primarySource, {120.25, -90.5}));
  const auto otherPhoto = app.nodePositions;
  app.sidecarWriteBlockedPath = app.path; ui.tick(app);
  CHECK(!ui::moveGraphNode(app, app.primarySource, {999, 999}));
  CHECK(samePositions(app.nodePositions, otherPhoto));
  app.sidecarWriteBlockedPath = app.path; app.nodePositions.clear(); ui.tick(app);
  CHECK(app.nodePositions.empty()); // Viewing a protected graph cannot materialize layout.
  app.nodePositions = otherPhoto;
  app.sidecarWriteBlockedPath.clear();
  CHECK(document_detail::openPhoto(app, a, {}, options)); ui.tick(app);
  CHECK(same(app.nodePositions.at(id.value), {-64.5, 80.125}));
  CHECK(same(app.nodePositions.at(app.primarySource.value), generated.at(app.primarySource.value)));
  CHECK(document_detail::openPhoto(app, b, {}, options)); ui.tick(app);
  CHECK(samePositions(app.nodePositions, otherPhoto));
  return true;
}

bool inspectorRawGammaEdit() {
  Files files;
  const auto photo = (files.dir / "synthetic.dng").string();
  const auto preferences = (files.dir / "preferences.json").string();
  // A tiny uncompressed linear DNG exercises the real LibRaw re-decode path,
  // without a camera fixture or a test replacement for the Inspector handler.
  auto *tiff = TIFFOpen(photo.c_str(), "w");
  CHECK(tiff);
  TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 32);
  TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 32);
  TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, 34892); // DNG LinearRaw RGB.
  TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 32);
  const uint8_t version[] = {1, 4, 0, 0};
  TIFFSetField(tiff, TIFFTAG_DNGVERSION, version);
  TIFFSetField(tiff, TIFFTAG_UNIQUECAMERAMODEL, "RawNode synthetic linear DNG");
  const float matrix[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  TIFFSetField(tiff, TIFFTAG_COLORMATRIX1, 9, matrix);
  TIFFSetField(tiff, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
  const uint32_t white = 65535;
  TIFFSetField(tiff, TIFFTAG_WHITELEVEL, 1, &white);
  std::vector<uint16_t> pixels(32 * 32 * 3, 12000);
  const auto written = TIFFWriteEncodedStrip(tiff, 0, pixels.data(), pixels.size() * sizeof(uint16_t));
  TIFFClose(tiff);
  CHECK(written > 0);

  App app;
  Preferences defaults; defaults.rawWorking = {RgbGamut::Rec2020, TransferFunction::Linear};
  std::string error;
  CHECK(savePreferences(preferences, defaults, error) && initialisePreferences(app, {preferences, ""}));
  const auto savedPreferences = bytes(preferences);
  CHECK(openPath(app, photo));
  CHECK(app.inputIsRaw && app.inputEncoding == defaults.rawWorking);
  CHECK(addNativeExposureNode(app));
  CHECK(setNodeParameterValue(app, *app.selectedNodeId(), "exposure", 1.0));
  app.renderer.start(); CHECK(idle(app));
  const auto previousPreview = app.preview;
  const float linear = app.full.px[0];
  CHECK(linear > 0.0031308f && linear < 1);
  const double encoded = 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  Ui ui; ui.showInspector = true;
  ui.tick(app); ui.tick(app); ui.click(app, ui.listSource);
  CHECK(app.selectedNodeId() == app.primarySource && ui.inspectorLastItem == ui.inspectorId("RAW gamma"));
  ui.click(app, {ui.inspectorLastRect.Min.x + 30, ui.inspectorLastRect.GetCenter().y});
  CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
  ui.tick(app); ui.tick(app); // Let the auto-sized popup settle before hit testing.
  // Linear is row 0, sRGB is row 1 in the actual combo popup.
  ImVec2 srgb{};
  for (const auto *window : ImGui::GetCurrentContext()->Windows)
    if (window->Active && (window->Flags & ImGuiWindowFlags_Popup))
      srgb = {window->DC.CursorStartPos.x + 20, window->DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() + 5};
  CHECK(srgb.x > 0); ui.click(app, srgb);
  const ColorEncoding changed{RgbGamut::Rec2020, TransferFunction::SRGB};
  CHECK(app.photoRawWorkingEncoding == changed && app.inputEncoding == changed);
  CHECK(app.rawWorkingEncoding == defaults.rawWorking && capturePreferences(app).rawWorking == defaults.rawWorking);
  CHECK(bytes(preferences) == savedPreferences);
  CHECK(app.preview != previousPreview && std::abs(app.full.px[0] - encoded) < 1e-5);
  CHECK(idle(app) && std::abs(app.preview->px[0] - encoded) < 1e-5);
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch > before.epoch);
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.processed.result.state == EvaluationState::Available);
    const auto rendered = std::get<ImageValue>(app.processed.result.value);
    CHECK(rendered && std::abs(rendered->px[0] - 2 * encoded) < 1e-5);
  }
  CHECK(saveCurrentInputSidecar(app));
  PersistFile saved;
  CHECK(readGraphSidecar(inputSidecarPath(photo), saved, error) == SidecarReadStatus::Loaded && saved.grade);
  CHECK(saved.grade->color.rawGamma == "srgb");
  CHECK(savePreferences(preferences, capturePreferences(app), error));
  Preferences reloaded; std::string warning;
  CHECK(loadPreferences(preferences, reloaded, warning) && warning.empty());
  CHECK(reloaded.rawWorking == defaults.rawWorking);
  return true;
}

bool inspectorContexts() {
  Ui ui; ui.showInspector = true; ui.captureLog = true;
  App app; app.path = "test.nef";
  app.full.w = app.full.h = 1; app.full.px = {0.25f, 0.25f, 0.25f, 1};
  app.preview = std::make_shared<Image>(app.full);
  CHECK(addNativeExposureNode(app)); const auto processor = *app.selectedNodeId();
  app.renderer.start(); CHECK(idle(app));
  std::atomic<int> mutations{0};
  RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
  const auto topology = graphJson(app);
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  ui.tick(app); ui.tick(app);
  CHECK(app.selectedNodeId() == processor && selectedNode(app)->processor);
  // Fixed List endpoints are outside the processor reorder projection.
  ui.click(app, ui.listSource);
  CHECK(app.selectedNodeId() == app.primarySource && !selectedNode(app));
  CHECK(ui.inspectorLastItem == 0); // Non-RAW input ends with information, no RAW widgets.
  CHECK(ui.inspectorLog.find("Input pixels:") != std::string::npos &&
        ui.inspectorLog.find("RAW gamma") == std::string::npos &&
        ui.inspectorLog.find("Search parameters") == std::string::npos);
  const float informationBottom = ui.inspectorLastRect.Max.y;
  app.inputIsRaw = true; ui.tick(app);
  CHECK(ui.inspectorLastItem == ui.inspectorId("RAW gamma"));
  CHECK(ui.inspectorLog.find("RAW colour space") != std::string::npos);
  CHECK(ui.inspectorLastRect.Max.y > informationBottom + ImGui::GetFrameHeight());
  CHECK(!(ui.inspectorItemFlags & ImGuiItemFlags_Disabled));
  app.sidecarWriteBlockedPath = app.path; ui.tick(app);
  CHECK(ui.inspectorItemFlags & ImGuiItemFlags_Disabled);
  app.sidecarWriteBlockedPath.clear();
  CHECK(!canCopySelectedNode(app) && !copySelectedNode(app) && !destroyNode(app, app.primarySource));
  ui.click(app, ui.listOutput);
  CHECK(app.outputSelected() && !app.selectedNodeId() && !selectedNode(app));
  CHECK(ui.inspectorLastItem == ui.inspectorId("JPEG quality"));
  CHECK(ui.inspectorLog.find("Output colour space") != std::string::npos &&
        ui.inspectorLog.find("Output gamma") != std::string::npos &&
        ui.inspectorLog.find("RAW gamma") == std::string::npos);
  CHECK(!canCopySelectedNode(app) && !copySelectedNode(app));
  ui.click(app, ui.graphButton); CHECK(app.outputSelected());
  ui.click(app, ui.listButton); CHECK(app.outputSelected());
  selectNode(app, std::nullopt); ui.tick(app);
  CHECK(!app.outputSelected() && !app.selectedNodeId() && ui.inspectorLastItem == 0);
  CHECK(ui.inspectorLog.find("Select Source, a processor or Output") != std::string::npos);
  CHECK(selectNode(app, processor)); app.paramFilter[0] = 'e';
  ui.click(app, ui.graphButton); ui.click(app, ui.listButton);
  CHECK(app.selectedNodeId() == processor && app.paramFilter[0] == 'e');
  CHECK(ui.inspectorLog.find("Exposure") != std::string::npos &&
        ui.inspectorLog.find("RAW gamma") == std::string::npos &&
        ui.inspectorLog.find("Output gamma") == std::string::npos);
  const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(after.epoch == before.epoch && after.previewPending == before.previewPending &&
        after.displayPending == before.displayPending && mutations == 0 && graphJson(app) == topology);
  // Application export preferences stay editable even on a protected photo.
  selectOutput(app); app.sidecarWriteBlockedPath = app.path; ui.tick(app);
  CHECK(!(ui.inspectorItemFlags & ImGuiItemFlags_Disabled));
  const auto quality = ui.inspectorLastRect;
  ui.click(app, {quality.Min.x + (quality.Max.x - quality.Min.x) * 0.3f, quality.GetCenter().y});
  CHECK(app.jpegQuality != 92 && capturePreferences(app).jpegQuality == app.jpegQuality);
  CHECK(captureGradeColor(app).outputColorSpace == "rec709");
  app.exportFormat = 0; ui.tick(app);
  CHECK(ui.inspectorLastItem == ui.inspectorId("Export format"));
  app.sidecarWriteBlockedPath.clear();
  CHECK(selectNode(app, app.primarySource));
  Node spare; spare.id = NodeId{"inspector-spare"};
  CHECK(graph::addNode(app, std::move(spare)).ok() && app.selectedNodeId() == app.primarySource);
  selectOutput(app);
  CHECK(destroyNode(app, NodeId{"inspector-spare"}) && app.outputSelected());
  selectNode(app, processor);
  CHECK(destroyNode(app, processor)); ui.tick(app);
  CHECK(!app.selectedNodeId() && !app.outputSelected() && ui.inspectorLastItem == 0);
  return true;
}

bool processorPickerInBothViews() {
  Ui ui; ui.captureLog = true; App app; app.path = "picker.png";
  CHECK(addNativeCstNode(app)); const auto initialOutput = app.documentOutput;
  CHECK(addNativeExposureNode(app)); const auto branch = *app.selectedNodeId();
  CHECK(graph::setDocumentOutput(app, initialOutput).ok());
  for (auto view : {NodesView::List, NodesView::Graph}) {
    app.nodesView = view;
    // Both presentations use the same selection-aware Add Processor action.
    for (int selection = 0; selection != 5; ++selection) {
      if (selection == 0) CHECK(selectNode(app, std::nullopt));
      else if (selection == 1) {
        CHECK(selectNode(app, std::nullopt));
        app.graphCanvas.framePending = true;
        ui.tick(app); ui.tick(app);
        if (view == NodesView::List) ui.click(app, ui.listSource);
        else {
          const auto position = app.nodePositions.at(app.primarySource.value);
          ui.click(app, ui.screen(app, {position.x + 60, position.y + 16}));
        }
        CHECK(app.selectedNodeId() == app.primarySource);
      } else if (selection == 2) CHECK(selectNode(app, app.nodes[1].id));
      else if (selection == 3) selectOutput(app);
      else CHECK(selectNode(app, branch));
      const auto previousOutput = app.documentOutput;
      const auto predecessor = selection == 1 ? app.primarySource : selection == 2 ? app.nodes[1].id :
                               selection == 4 ? branch : previousOutput.nodeId;
      std::optional<NodeId> successor;
      if (selection == 1 || selection == 2) {
        // The fixture has one off-chain consumer; it must never be spliced.
        for (const auto &node : app.nodes)
          if (node.operation != NodeOperation::Source && node.id != branch &&
              node.fixedInputs.at(ports::image) == (OutputRef{predecessor, ports::image}))
            successor = node.id;
      }
      const auto count = app.nodes.size();
      std::snprintf(app.pluginFilter, sizeof app.pluginFilter, "Exposure");
      ui.tick(app); ui.tick(app); ui.click(app, ui.addButton);
      CHECK(ui.pickerOpen);
      CHECK(ui.nodesLog.find("Exposure") != std::string::npos && ui.nodesLog.substr(0, ui.nodesLog.find("Source")).find("CST") == std::string::npos);
      ImVec2 choice{};
      for (const auto *window : ImGui::GetCurrentContext()->Windows)
        if (window->Active && (window->Flags & ImGuiWindowFlags_Popup))
          choice = {window->DC.CursorStartPos.x + 30, window->DC.CursorPosPrevLine.y + 6};
      CHECK(choice.x > 0);
      ui.click(app, choice);
      CHECK(!ui.pickerOpen && app.nodes.size() == count + 1);
      const auto *node = selectedNode(app);
      CHECK(node && node->processor && nodeDisplayName(*node) == "Exposure");
      CHECK(node->fixedInputs.at(ports::image) == (OutputRef{predecessor, ports::image}));
      if (successor) {
        CHECK(findNode(app, *successor)->fixedInputs.at(ports::image) == (OutputRef{node->id, ports::image}));
        CHECK(app.documentOutput == previousOutput);
      } else if (selection == 4) CHECK(app.documentOutput == previousOutput);
      else CHECK(app.documentOutput == (OutputRef{node->id, ports::image}));
      CHECK(app.pluginFilter[0] == '\0');
      CHECK(findNode(app, branch)->fixedInputs.at(ports::image) == initialOutput);
    }
  }
  app.nodesView = NodesView::List; ui.tick(app);
  const float wideSourceY = ui.listSource.y;
  ui.nodesWidth = 220; ui.tick(app); ui.tick(app);
  CHECK(ui.listSource.y >= wideSourceY + ImGui::GetFrameHeightWithSpacing());
  ui.nodesWidth = 1150; app.nodesView = NodesView::Graph; ui.tick(app); ui.tick(app);
  std::snprintf(app.pluginFilter, sizeof app.pluginFilter, "CTL");
  ui.click(app, ui.addButton); CHECK(ui.pickerOpen);
  CHECK(ui.nodesLog.find("Load CTL script") != std::string::npos &&
        ui.nodesLog.find("Exposure") == std::string::npos);
  std::snprintf(app.pluginFilter, sizeof app.pluginFilter, "no-such-processor-xyz"); ui.tick(app);
  CHECK(ui.nodesLog.find("No matches") != std::string::npos);
  return true;
}

bool workspaceDocking() {
  std::string saved;
  ImGuiID previewId = 0, inspectorId = 0;
  {
    Ui ui; ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    App app;
    for (int i = 0; i < 5; ++i) {
      auto &io = ImGui::GetIO(); io.DisplaySize = {1280, 800}; io.DeltaTime = 1.0f / 60;
      ImGui::NewFrame(); DrawUiFrame(app); ImGui::EndFrame();
    }
    const auto *preview = ImGui::FindWindowByName(DockLayout::kPreview);
    const auto *nodes = ImGui::FindWindowByName(DockLayout::kNodes);
    const auto *inspector = ImGui::FindWindowByName(DockLayout::kInspector);
    const auto *strip = ImGui::FindWindowByName(DockLayout::kFilmstrip);
    CHECK(preview && nodes && inspector && strip);
    CHECK(!ImGui::FindWindowByName("OFX Plugins") && !ImGui::FindWindowByName("OFX Parameters"));
    CHECK(nodes->Pos.y >= preview->Pos.y + preview->Size.y);
    CHECK(inspector->Pos.x >= preview->Pos.x + preview->Size.x);
    CHECK(strip->Pos.y >= nodes->Pos.y + nodes->Size.y && strip->Pos.x == preview->Pos.x);
    CHECK(strip->Size.x > preview->Size.x + inspector->Size.x);
    // Create a new custom arrangement, including a floating Nodes window.
    ImGui::NewFrame();
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    ImGui::SetNextWindowPos({100, 140}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({550, 280}, ImGuiCond_Always);
    ImGui::Begin(DockLayout::kNodes); ImGui::End(); ImGui::EndFrame();
    previewId = preview->DockId; inspectorId = inspector->DockId;
    size_t length = 0; const char *ini = ImGui::SaveIniSettingsToMemory(&length);
    saved.assign(ini, length);
  }
  {
    Ui ui; ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::LoadIniSettingsFromMemory(saved.c_str()); App app;
    for (int i = 0; i < 5; ++i) {
      auto &io = ImGui::GetIO(); io.DisplaySize = {1280, 800}; io.DeltaTime = 1.0f / 60;
      ImGui::NewFrame(); DrawUiFrame(app); ImGui::EndFrame();
    }
    const auto *nodes = ImGui::FindWindowByName(DockLayout::kNodes);
    CHECK(nodes && !nodes->DockId && nodes->Size.x == 550 && nodes->Size.y == 280);
    CHECK(ImGui::FindWindowByName(DockLayout::kPreview)->DockId == previewId);
    CHECK(ImGui::FindWindowByName(DockLayout::kInspector)->DockId == inspectorId);
  }
  return true;
}

bool firstDockedFrame() {
  Ui ui; ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  App app; app.nodesView = NodesView::Graph;
  CHECK(addNativeExposureNode(app) && addNativeExposureNode(app));
  // Exercise the real frame order: Nodes is initially drawn before Preview,
  // and DockBuilder geometry settles after the first frame.
  for (int frame = 0; frame < 5; ++frame) {
    auto &io = ImGui::GetIO(); io.DisplaySize = {1400, 900}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame(); DrawUiFrame(app); ImGui::EndFrame();
  }
  CHECK(!app.graphCanvas.framePending);
  const auto layout = ui::layoutGraph(graph(app), app.nodePositions);
  const auto size = app.graphCanvas.lastCanvasSize;
  const auto inFrame = [&](PersistPosition point) {
    const auto canvas = ui::graphToCanvas(app.graphCanvas.view, point);
    return canvas.x >= 31 && canvas.y >= 31 && canvas.x <= size.x - 31 && canvas.y <= size.y - 31;
  };
  for (const auto &entry : layout.nodes) {
    CHECK(inFrame(entry.second.position));
    CHECK(inFrame({entry.second.position.x + ui::graphNodeWidth, entry.second.position.y + entry.second.height}));
  }
  CHECK(inFrame(layout.output) && inFrame({layout.output.x + ui::graphOutputWidth,
                                          layout.output.y + ui::graphOutputHeight}));
  return true;
}

// Independent expected bindings and +1 EV arithmetic exercise actual UI commands;
// expected topology is never derived from the drawing or command implementation.
OutputRef imageOf(const NodeId &id) { return {id, ports::image}; }
InputAddress imageInput(const NodeId &id) { return {id, std::nullopt, ports::image}; }
bool consumes(const App &app, const NodeId &consumer, const NodeId &producer) {
  const auto *binding = findInput(app, imageInput(consumer));
  return binding && *binding == imageOf(producer);
}
void key(Ui &ui, App &app, ImGuiKey value) {
  ImGui::GetIO().AddKeyEvent(value, true); ui.tick(app);
  ImGui::GetIO().AddKeyEvent(value, false); ui.tick(app);
}
void rightClick(Ui &ui, App &app, ImVec2 point) {
  ui.hover(app, point); ImGui::GetIO().AddMouseButtonEvent(1, true); ui.tick(app);
  ImGui::GetIO().AddMouseButtonEvent(1, false); ui.tick(app);
}
ImVec2 lastPopupItem() {
  for (const auto *window : ImGui::GetCurrentContext()->Windows)
    if (window->Active && (window->Flags & ImGuiWindowFlags_Popup))
      return {window->DC.CursorStartPos.x + 30, window->DC.CursorPosPrevLine.y + 6};
  return {};
}

bool wireEditingFrames() {
  for (const double zoom : {0.2, 0.5, 1.0, 2.5}) {
    Ui ui; App app; app.path = "wires"; app.nodesView = NodesView::Graph;
    CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
    CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId();
    CHECK(graph::disconnect(app, imageInput(a)).ok() && graph::disconnect(app, imageInput(b)).ok());
    ui.tick(app); ui.tick(app);
    app.nodePositions = {{app.primarySource.value, {-200, 0}}, {a.value, {100, 0}}, {b.value, {400, 0}}};
    app.graphCanvas.framePending = false; app.graphCanvas.view = {{20, 50}, zoom}; ui.tick(app);
    const auto positions = app.nodePositions; const auto pan = app.graphCanvas.view.pan;
    std::atomic<int> mutations{0}; RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
    const auto source = ui.screen(app, {40, 44}), aIn = ui.screen(app, {100, 44});
    const auto aOut = ui.screen(app, {340, 44}), bIn = ui.screen(app, {400, 44});
    auto before = graphJson(app);
    // A horizontal halo outside the small rendered port still wins over body
    // dragging, at both zoom extremes. Topology stays unchanged until release.
    ui.hover(app, {source.x + 5, source.y});
    ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app);
    CHECK(app.graphCanvas.wire && !app.graphCanvas.dragging);
    ui.hover(app, aIn); CHECK(graphJson(app) == before);
    ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app);
    CHECK(consumes(app, a, app.primarySource) && mutations == 1 && !app.graphCanvas.wire);
    ui.drag(app, aOut, {bIn.x - aOut.x, bIn.y - aOut.y}, 0);
    CHECK(consumes(app, b, a) && mutations == 2);
    // Input-origin drag and occupied-input replacement are single commits.
    ui.drag(app, bIn, {source.x - bIn.x, source.y - bIn.y}, 0);
    CHECK(consumes(app, b, app.primarySource) && mutations == 3 && consumes(app, a, app.primarySource));
    ui.drag(app, aOut, {bIn.x - aOut.x, bIn.y - aOut.y}, 0);
    CHECK(consumes(app, b, a) && mutations == 4);
    before = graphJson(app); const auto epoch = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
    ui.drag(app, aOut, {bIn.x - aOut.x, bIn.y - aOut.y}, 0); // Same edge.
    ui.drag(app, aOut, {0, 150}, 0); // Empty drop.
    ui.drag(app, source, {aOut.x - source.x, aOut.y - source.y}, 0); // Output/output.
    ui.drag(app, aOut, {aIn.x - aOut.x, aIn.y - aOut.y}, 0); // Self cycle.
    CHECK(app.getStatus().find("Cycle") != std::string::npos || app.getStatus().find("cycle") != std::string::npos);
    CHECK(graphJson(app) == before && mutations == 4);
    CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == epoch);
    // Escape, view change, missing endpoint and photograph change cancel.
    ui.hover(app, source); ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app);
    key(ui, app, ImGuiKey_Escape); CHECK(!app.graphCanvas.wire);
    ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app);
    app.graphCanvas.wire = ui::GraphPortAddress{app.primarySource, {}, ports::image, PortDirection::Output};
    app.nodesView = NodesView::List; ui.tick(app); CHECK(!app.graphCanvas.wire);
    app.nodesView = NodesView::Graph;
    app.graphCanvas.wire = ui::GraphPortAddress{NodeId{"absent"}, {}, ports::image, PortDirection::Output};
    ui.tick(app); CHECK(!app.graphCanvas.wire);
    app.graphCanvas.wire = ui::GraphPortAddress{app.primarySource, {}, ports::image, PortDirection::Output};
    app.path = "different-photo"; ui.tick(app); CHECK(!app.graphCanvas.wire);
    CHECK(graphJson(app) == before && mutations == 4 && samePositions(app.nodePositions, positions));
    // Restore viewport following the deliberate photo-switch reset.
    app.graphCanvas.framePending = false; app.graphCanvas.view = {pan, zoom}; ui.tick(app);
    rightClick(ui, app, bIn); ui.click(app, lastPopupItem());
    CHECK(!*findInput(app, imageInput(b)) && mutations == 5);
    CHECK(!requestedGraphError(graph(app), app.documentOutput).empty());
    ui.drag(app, aOut, {bIn.x - aOut.x, bIn.y - aOut.y}, 0);
    CHECK(consumes(app, b, a) && mutations == 6 && requestedGraphError(graph(app), app.documentOutput).empty());
    CHECK(samePositions(app.nodePositions, positions) && same(app.graphCanvas.view.pan, pan));
    RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  }
  return true;
}

bool outputAndTypedWireFrames() {
  Ui ui; App app; app.path = "typed-wires"; app.nodesView = NodesView::Graph;
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
  app.full.w = app.full.h = 1; app.full.px = {0.25f, 0.25f, 0.25f, 1};
  app.preview = std::make_shared<Image>(app.full);
  CHECK(setNodeParameterValue(app, a, "exposure", 1.0));
  Node mask(NodeOperation::TestImageToMask); mask.id = NodeId{"mask"};
  CHECK(graph::addNode(app, std::move(mask)).ok());
  Node consumer(NodeOperation::TestMaskProcessor); consumer.id = NodeId{"mask-consumer"};
  CHECK(graph::addNode(app, std::move(consumer)).ok());
  ui.tick(app); ui.tick(app); app.graphCanvas.framePending = false;
  app.nodePositions = {{app.primarySource.value, {0, 0}}, {a.value, {320, 0}}, {"mask", {320, 180}}, {"mask-consumer", {640, 180}}};
  app.graphCanvas.view = {{20, 40}, 0.8}; ui.tick(app);
  const auto layout = ui::layoutGraph(graph(app), app.nodePositions);
  const auto output = ui.screen(app, {layout.output.x, layout.output.y + 48});
  const auto source = ui.screen(app, {240, 44}), aIn = ui.screen(app, {320, 44});
  const auto aOut = ui.screen(app, {560, 44}), maskOut = ui.screen(app, {560, 224});
  const auto encoding = app.outputEncoding;
  std::atomic<int> mutations{0}; RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
  ui.drag(app, source, {output.x - source.x, output.y - source.y}, 0);
  CHECK(app.documentOutput == imageOf(app.primarySource) && mutations == 1);
  for (auto purpose : {EvaluationPurpose::Preview, EvaluationPurpose::Export}) {
    auto result = app.renderer.evaluateOwned(app.documentOutput, {purpose, 1, 1, 1, 1}, app.preview);
    CHECK(result.state == EvaluationState::Available && std::get<ImageValue>(result.value)->px[0] == 0.25f);
  }
  ui.drag(app, output, {aOut.x - output.x, aOut.y - output.y}, 0);
  CHECK(app.documentOutput == imageOf(a) && mutations == 2);
  for (auto purpose : {EvaluationPurpose::Preview, EvaluationPurpose::Export}) {
    auto result = app.renderer.evaluateOwned(app.documentOutput, {purpose, 1, 1, 1, 1}, app.preview);
    CHECK(result.state == EvaluationState::Available && std::get<ImageValue>(result.value)->px[0] == 0.5f);
  }
  ui.drag(app, maskOut, {aIn.x - maskOut.x, aIn.y - maskOut.y}, 0);
  CHECK(consumes(app, a, app.primarySource) && mutations == 2 && app.getStatus().find("type mismatch") != std::string::npos);
  ui.drag(app, maskOut, {output.x - maskOut.x, output.y - maskOut.y}, 0);
  CHECK(app.documentOutput == imageOf(a) && mutations == 2 && app.outputEncoding == encoding);
  const auto maskIn = ui.screen(app, {640, 224});
  const auto maskConsumerOut = ui.screen(app, {880, 224});
  const InputAddress maskInput{NodeId{"mask-consumer"}, {}, ports::mask};
  const OutputRef maskOutput{NodeId{"mask"}, ports::mask};
  ui.drag(app, maskOut, {maskIn.x - maskOut.x, maskIn.y - maskOut.y}, 0);
  CHECK(*findInput(app, maskInput) == maskOutput && mutations == 3);
  ui.drag(app, maskIn, {maskOut.x - maskIn.x, maskOut.y - maskIn.y}, 0); // Same Mask edge.
  CHECK(mutations == 3);
  ui.drag(app, maskConsumerOut, {maskIn.x - maskConsumerOut.x, maskIn.y - maskConsumerOut.y}, 0);
  CHECK(*findInput(app, maskInput) == maskOutput && mutations == 3); // Mask self-cycle.
  rightClick(ui, app, maskIn); ui.click(app, lastPopupItem());
  CHECK(!*findInput(app, maskInput) && mutations == 4);
  const auto bindings = findNode(app, a)->fixedInputs;
  app.sidecarWriteBlockedPath = app.path;
  const auto positions = app.nodePositions;
  ui.drag(app, source, {output.x - source.x, output.y - source.y}, 0);
  ui.click(app, ui.screen(app, {360, 16})); key(ui, app, ImGuiKey_Delete);
  CHECK(app.documentOutput == imageOf(a) && findNode(app, a) && mutations == 4);
  CHECK(samePositions(app.nodePositions, positions));
  app.sidecarWriteBlockedPath.clear();
  {
    auto owner = app.renderer.acquireExport(); CHECK(owner);
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    ui.drag(app, source, {output.x - source.x, output.y - source.y}, 0);
    ui.click(app, ui.screen(app, {360, 16})); key(ui, app, ImGuiKey_Backspace);
    rightClick(ui, app, ui.screen(app, {360, 16})); ui.click(app, lastPopupItem());
    CHECK(app.documentOutput == imageOf(a) && findNode(app, a) && mutations == 4);
    CHECK(findNode(app, a)->fixedInputs == bindings);
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(after.exportBusy && before.epoch == after.epoch && before.mutationDepth == after.mutationDepth);
    CHECK(before.previewPending == after.previewPending && before.displayPending == after.displayPending);
    key(ui, app, ImGuiKey_Escape);
  }
  RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  return true;
}

bool deletionFrames() {
  Ui ui; ui.showOtherPanel = true; ui.showInspector = true; ui.captureLog = true;
  App app; app.path = "delete"; app.nodesView = NodesView::Graph;
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
  CHECK(addNativeCstNode(app)); const auto b = *app.selectedNodeId();
  ui.tick(app); ui.tick(app); app.graphCanvas.framePending = false;
  app.nodePositions = {{app.primarySource.value, {0, 0}}, {a.value, {280, 0}}, {b.value, {560, 0}}};
  app.graphCanvas.view = {{20, 30}, 0.8}; ui.tick(app);
  const auto aBody = ui.screen(app, {350, 16});
  ui.click(app, aBody); CHECK(app.selectedNodeId() == a);
  ui.click(app, ui.textInput); key(ui, app, ImGuiKey_Delete); key(ui, app, ImGuiKey_Backspace);
  CHECK(findNode(app, a));
  ui.click(app, ui.otherButton); key(ui, app, ImGuiKey_Delete); CHECK(findNode(app, a));
  ui.click(app, aBody); ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true); ui.tick(app);
  CHECK(!findNode(app, a) && !app.selectedNodeId() && !app.nodePositions.count(a.value));
  CHECK(ui.inspectorLog.find("Select Source, a processor or Output") != std::string::npos);
  CHECK(consumes(app, b, app.primarySource));
  // Even if a new node becomes selected, holding Delete cannot remove it.
  CHECK(selectNode(app, b)); for (int i = 0; i < 60; ++i) ui.tick(app);
  CHECK(findNode(app, b));
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false); ui.tick(app);
  key(ui, app, ImGuiKey_Backspace);
  CHECK(!findNode(app, b) && app.documentOutput == imageOf(app.primarySource) && !selectedNode(app));
  ui.click(app, ui.screen(app, {60, 16})); key(ui, app, ImGuiKey_Delete); CHECK(app.nodes.size() == 1);
  const auto output = ui::layoutGraph(graph(app), app.nodePositions).output;
  ui.click(app, ui.screen(app, {output.x + 30, output.y + 16}));
  key(ui, app, ImGuiKey_Backspace); CHECK(app.nodes.size() == 1 && app.outputSelected());
  CHECK(addNativeExposureNode(app)); const auto c = *app.selectedNodeId();
  CHECK(graph::setDocumentOutput(app, imageOf(app.primarySource)).ok()); // Off-spine deletion.
  ui.tick(app);
  const auto pos = app.nodePositions.at(c.value);
  rightClick(ui, app, ui.screen(app, {pos.x + 50, pos.y + 16}));
  ui.click(app, lastPopupItem()); CHECK(!findNode(app, c) && app.nodes.size() == 1);
  return true;
}



bool listDeletionFrames() {
  for (auto pressed : {ImGuiKey_Delete, ImGuiKey_Backspace}) {
    Ui ui; ui.showInspector = true; ui.captureLog = true;
    App app; app.path = "list-delete"; app.nodesView = NodesView::List;
    app.full.w = app.full.h = 1; app.full.px = {0.25f, 0.25f, 0.25f, 1};
    app.preview = std::make_shared<Image>(app.full);
    CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
    CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId();
    CHECK(setNodeParameterValue(app, a, "exposure", 1.0));
    CHECK(setNodeParameterValue(app, b, "exposure", 1.0));
    Node branch(NodeOperation::TestImage); branch.id = NodeId{"fan-out"};
    branch.fixedInputs.at(ports::image) = imageOf(a);
    CHECK(graph::addNode(app, std::move(branch)).ok());
    app.nodePositions = {{a.value, {-12, 7}}, {b.value, {321, 19}}};
    app.renderer.start(); CHECK(idle(app));
    ui.tick(app); ui.tick(app);
    // Source and the unwrapped document-output label occupy two text lines;
    // the main-spine heading occupies one frame-height line before a's row.
    // This small fixture fits without scrolling or wrapping; off-chain rows follow b.
    const auto aRow = ImVec2{ui.listSource.x, ui.listSource.y +
      2 * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing()};
    ui.click(app, aRow); CHECK(app.selectedNodeId() == a && ui.listFocused());
    app.paramFilter[0] = 'x';
    std::atomic<int> mutations{0}; RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    key(ui, app, pressed); CHECK(idle(app));
    CHECK(!findNode(app, a) && !app.selectedNodeId() && app.paramFilter[0] == '\0');
    CHECK(consumes(app, b, app.primarySource) && consumes(app, NodeId{"fan-out"}, app.primarySource));
    CHECK(app.documentOutput == imageOf(b) && documentStateError(app).empty());
    CHECK(app.nodePositions.size() == 1 && same(app.nodePositions.at(b.value), {321, 19}));
    CHECK(ui.inspectorLog.find("Select Source, a processor or Output") != std::string::npos);
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(mutations == 1 && after.epoch > before.epoch && after.mutationDepth == 0);
    // Source 0.25 with only b's +1 stop remaining must evaluate to 0.5.
    {
      std::lock_guard<std::mutex> lock(app.displayMutex);
      CHECK(app.processed.result.requested == imageOf(b));
      CHECK(app.processed.result.state == EvaluationState::Available);
      CHECK(std::get<ImageValue>(app.processed.result.value)->px[0] == 0.5f);
    }
    // Off-chain nodes use the same splice rules; deleting the tail updates Output.
    ui.click(app, ui.listProcessorFromEnd()); CHECK(app.selectedNodeId() == NodeId{"fan-out"});
    key(ui, app, pressed); CHECK(!findNode(app, NodeId{"fan-out"}) && app.documentOutput == imageOf(b));
    ui.click(app, ui.listProcessorFromEnd()); key(ui, app, pressed);
    CHECK(app.nodes.size() == 1 && app.documentOutput == imageOf(app.primarySource) && mutations == 3);
    RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  }
  return true;
}

bool listDeletionSafetyFrames() {
  for (auto pressed : {ImGuiKey_Delete, ImGuiKey_Backspace}) {
    Ui ui; ui.showOtherPanel = true;
    App app; app.path = "list-delete-safety"; app.nodesView = NodesView::List;
    CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
    CHECK(addNativeCstNode(app)); const auto b = *app.selectedNodeId();
    ui.tick(app); ui.tick(app);
    ui.click(app, ui.listProcessorFromEnd(1)); CHECK(app.selectedNodeId() == a && ui.listFocused());
    const auto topology = graphJson(app);
    const auto positions = app.nodePositions;
    std::atomic<int> mutations{0}; RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    ui.click(app, ui.textInput); CHECK(ImGui::GetIO().WantTextInput);
    CHECK(app.selectedNodeId() == a && !ui.listFocused());
    CHECK(ui.context->NavWindow == ImGui::FindWindowByName("Other panel"));
    key(ui, app, pressed);
    CHECK(graphJson(app) == topology && mutations == 0);
    ui.click(app, ui.otherButton); CHECK(!ImGui::GetIO().WantTextInput);
    CHECK(app.selectedNodeId() == a && !ui.listFocused());
    CHECK(ui.context->NavWindow == ImGui::FindWindowByName("Other panel"));
    key(ui, app, pressed);
    CHECK(graphJson(app) == topology && mutations == 0);
    ui.click(app, ui.listProcessorFromEnd(1));
    for (auto modifier : {ImGuiMod_Ctrl, ImGuiMod_Super, ImGuiMod_Alt, ImGuiMod_Shift}) {
      ImGui::GetIO().AddKeyEvent(modifier, true); ui.tick(app);
      CHECK(app.selectedNodeId() == a && ui.listFocused() && app.renderer.canEditParameters());
      CHECK(!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive());
      CHECK(ImGui::GetIO().KeyMods != 0);
      key(ui, app, pressed);
      CHECK(graphJson(app) == topology && mutations == 0);
      ImGui::GetIO().AddKeyEvent(modifier, false); ui.tick(app);
    }
    app.sidecarWriteBlockedPath = app.path;
    CHECK(app.selectedNodeId() == a && ui.listFocused() && app.isPhotoProtected());
    CHECK(!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive());
    key(ui, app, pressed);
    CHECK(graphJson(app) == topology && mutations == 0);
    app.sidecarWriteBlockedPath.clear();
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(after.epoch == before.epoch && after.mutationDepth == before.mutationDepth);
    CHECK(after.previewPending == before.previewPending && after.displayPending == before.displayPending);
    {
      auto owner = app.renderer.acquireExport(); CHECK(owner);
      const auto exporting = RenderRuntimeTestAccess::snapshot(app.renderer);
      CHECK(app.selectedNodeId() == a && ui.listFocused() && exporting.exportBusy);
      CHECK(!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive());
      key(ui, app, pressed);
      CHECK(graphJson(app) == topology && mutations == 0);
      const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
      CHECK(after.exportBusy && after.epoch == exporting.epoch && after.mutationDepth == exporting.mutationDepth);
      CHECK(after.previewPending == exporting.previewPending && after.displayPending == exporting.displayPending);
    }
    CHECK(graphJson(app) == topology && samePositions(app.nodePositions, positions) && mutations == 0);
    // Fixed endpoints and empty/stale selections cannot become deletion targets.
    ui.click(app, ui.listSource); CHECK(app.selectedNodeId() == app.primarySource && ui.listFocused());
    key(ui, app, pressed);
    ui.click(app, ui.listOutput); CHECK(app.outputSelected() && ui.listFocused());
    key(ui, app, pressed); CHECK(app.outputSelected());
    CHECK(selectNode(app, std::nullopt) && ui.listFocused()); key(ui, app, pressed);
    app.selection = NodeId{"stale"}; CHECK(ui.listFocused()); key(ui, app, pressed);
    CHECK(graphJson(app) == topology && mutations == 0);
    // A held key must not delete a newly selected processor, even past repeat delay.
    ui.click(app, ui.listProcessorFromEnd(1));
    CHECK(app.selectedNodeId() == a && ui.listFocused());
    ImGui::GetIO().AddKeyEvent(pressed, true); ui.tick(app);
    CHECK(!findNode(app, a) && findNode(app, b) && mutations == 1);
    ui.tick(app); // Refresh row geometry after the deferred removal, with the key still held.
    ui.click(app, ui.listProcessorFromEnd()); CHECK(app.selectedNodeId() == b && ui.listFocused());
    for (int i = 0; i < 60; ++i) ui.tick(app);
    CHECK(findNode(app, b) && mutations == 1);
    ImGui::GetIO().AddKeyEvent(pressed, false); ui.tick(app);
    RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  }
  return true;
}

bool listPostDeletionHighlightFrames() {
  for (bool keyboardNavigation : {false, true}) {
    for (auto pressed : {ImGuiKey_Delete, ImGuiKey_Backspace}) {
      Ui ui; ui.showInspector = true; ui.captureLog = true;
      if (keyboardNavigation) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
      // Give Selectable's hover fill a unique colour so the draw-data check
      // cannot confuse it with text, buttons, borders or other theme colours.
      ImGui::GetStyle().Colors[ImGuiCol_HeaderHovered] = {0.81f, 0.19f, 0.63f, 0.91f};
      App app; app.nodesView = NodesView::List;
      CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
      CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId();
      ui.tick(app); ui.tick(app);
      const auto firstRow = ui.listProcessorFromEnd(1);
      ui.click(app, firstRow); CHECK(app.selectedNodeId() == a && ui.listFocused());
      const auto hoverColour = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
      const auto hasHoverFill = [&] {
        const auto &vertices = ui.context->NavWindow->DrawList->VtxBuffer;
        return std::any_of(vertices.begin(), vertices.end(), [&](const ImDrawVert &vertex) {
          return vertex.col == hoverColour;
        });
      };
      CHECK(hasHoverFill());
      key(ui, app, pressed);
      CHECK(!findNode(app, a) && !app.selectedNodeId() && ui.listFocused());
      const auto remainingRow = ui.listProcessorFromEnd();
      CHECK(firstRow.x == remainingRow.x && firstRow.y == remainingRow.y);
      const auto scope = ui.context->NavWindow->GetID(b.value.c_str());
      const auto remainingRowId = ImHashStr(nodeDisplayName(*findNode(app, b)).c_str(), 0, scope);
      // Check the actual hover background submitted for drawing, plus navigation
      // feedback: neither depends on RawNode's selectedNodeId().
      for (int frame = 0; frame < 3; ++frame) {
        CHECK(!hasHoverFill());
        CHECK(ui.context->NavId != remainingRowId || !ui.context->NavCursorVisible);
        ui.tick(app);
      }
      CHECK(ui.inspectorLog.find("Select Source, a processor or Output") != std::string::npos);
      const auto unchanged = graphJson(app);
      for (auto next : {ImGuiKey_Delete, ImGuiKey_Backspace}) {
        key(ui, app, next); CHECK(graphJson(app) == unchanged && !app.selectedNodeId() && ui.listFocused());
        CHECK(!hasHoverFill());
      }
      CHECK(ui.inspectorLog.find("Select Source, a processor or Output") != std::string::npos);
      if (keyboardNavigation) {
        // Deliberate pointer movement restores ordinary hover without selecting.
        ui.hover(app, {remainingRow.x + 2, remainingRow.y});
        CHECK(hasHoverFill() && !app.selectedNodeId());
        key(ui, app, ImGuiKey_DownArrow);
        CHECK(ui.context->NavId && ui.context->NavIdIsAlive && ui.context->NavCursorVisible);
        CHECK(!app.selectedNodeId()); // Explicit navigation still works; it does not select a processor.
      }
      // In the default configuration, no mouse movement has occurred since deletion.
      ui.click(app, remainingRow); CHECK(app.selectedNodeId() == b && ui.listFocused());
      CHECK(ui.context->NavId == remainingRowId);
      key(ui, app, pressed); CHECK(!findNode(app, b) && !app.selectedNodeId());
    }
  }
  return true;
}

bool listDeletionRulesAndViewSwitching() {
  Ui ui;
  App app; app.path = "list-delete-rules"; app.nodesView = NodesView::List;
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
  ui.tick(app); ui.tick(app); ui.click(app, ui.listProcessorFromEnd());
  for (auto pressed : {ImGuiKey_Delete, ImGuiKey_Backspace}) {
    ui.click(app, ui.graphButton); key(ui, app, pressed);
    CHECK(app.nodesView == NodesView::Graph && findNode(app, a) && app.selectedNodeId() == a);
    ui.click(app, ui.listButton); key(ui, app, pressed);
    CHECK(app.nodesView == NodesView::List && findNode(app, a) && app.selectedNodeId() == a);
  }
  // Disconnected document Output cannot be deleted without a replacement.
  CHECK(graph::disconnect(app, {a, {}, ports::image}).ok());
  ui.tick(app); ui.click(app, ui.listProcessorFromEnd());
  const auto before = graphJson(app);
  const auto state = RenderRuntimeTestAccess::snapshot(app.renderer);
  key(ui, app, ImGuiKey_Delete);
  CHECK(graphJson(app) == before && app.selectedNodeId() == a);
  CHECK(app.getStatus().find("explicit replacement") != std::string::npos);
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == state.epoch);
  // Once off-chain, generic deletion is permitted and disconnects consumers.
  CHECK(graph::setDocumentOutput(app, imageOf(app.primarySource)).ok());
  Node consumer(NodeOperation::TestImage); consumer.id = NodeId{"consumer"};
  consumer.fixedInputs.at(ports::image) = imageOf(a);
  CHECK(graph::addNode(app, std::move(consumer)).ok());
  // Off-chain rows sort by NodeId: consumer precedes the generated processor ID.
  ui.tick(app); ui.click(app, ui.listProcessorFromEnd()); CHECK(app.selectedNodeId() == a);
  key(ui, app, ImGuiKey_Backspace);
  CHECK(!findNode(app, a) && !*findInput(app, {NodeId{"consumer"}, {}, ports::image}));
  CHECK(app.documentOutput == imageOf(app.primarySource));
  // Exercise the existing X control through its actual click path as well.
  ui.click(app, ui.listProcessorFromEnd(0, true));
  CHECK(app.nodes.size() == 1 && documentStateError(app).empty());
  return true;
}

bool gestureEndpointLifecycle() {
  Ui ui; App app; app.path = "gesture-photo-a"; app.nodesView = NodesView::Graph;
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId();
  ui.tick(app); ui.tick(app); app.graphCanvas.framePending = false;
  app.nodePositions = {{app.primarySource.value, {0, 0}}, {a.value, {320, 0}}, {b.value, {640, 0}}};
  app.graphCanvas.view = {{20, 40}, 0.6}; ui.tick(app);
  const auto begin = [&](PersistPosition point) {
    ui.hover(app, ui.screen(app, point));
    ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app);
  };
  const auto release = [&] { ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app); };
  begin({560, 44}); CHECK(app.graphCanvas.wire && app.graphCanvas.wire->nodeId == a);
  CHECK(destroyNode(app, a));
  CHECK(app.graphCanvas.wire); // Graph command has no canvas-state dependency.
  ui.hover(app, ui.screen(app, {640, 44})); CHECK(!app.graphCanvas.wire);
  release(); CHECK(consumes(app, b, app.primarySource));
  const auto before = graphJson(app);
  begin({240, 44}); CHECK(app.graphCanvas.wire);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); ui.tick(app); CHECK(!app.graphCanvas.wire);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); release();
  begin({240, 44}); CHECK(app.graphCanvas.wire);
  app.nodesView = NodesView::List; ui.tick(app); CHECK(!app.graphCanvas.wire);
  app.nodesView = NodesView::Graph; release();
  begin({240, 44}); CHECK(app.graphCanvas.wire);
  app.path = "gesture-photo-b"; ui.tick(app); CHECK(!app.graphCanvas.wire); release();
  CHECK(graphJson(app) == before);
  // A menu retains only its original identity when another edit removes it.
  const auto pos = app.nodePositions.at(b.value);
  rightClick(ui, app, ui.screen(app, {pos.x + 60, pos.y + 16}));
  CHECK(app.graphCanvas.contextNode == b);
  CHECK(destroyNode(app, b)); ui.tick(app);
  CHECK(app.nodes.size() == 1 && !selectedNode(app));
  return true;
}

bool contextMenusAfterGradePaste() {
  for (const bool inputMenu : {false, true}) for (const bool mac : {false, true}) {
    Ui ui; App app; app.path = "same-photo"; app.nodesView = NodesView::Graph;
    app.full.w = app.full.h = 1; app.full.px = {0.25f, 0.25f, 0.25f, 1};
    app.preview = std::make_shared<Image>(app.full);
    CHECK(addNativeExposureNode(app)); const auto reused = *app.selectedNodeId();
    CHECK(selectedNode(app)->processor->identifier() == "rawnode.native.exposure");
    App incoming; CHECK(addNativeCstNode(incoming));
    incoming.nodes.back().id = reused; incoming.documentOutput = imageOf(reused);
    incoming.nodePositions = {{incoming.primarySource.value, {0, 0}}, {reused.value, {320, 0}}};
    CHECK(copyGrade(incoming)); // Real Full Grade clipboard payload, not fragment Paste.
    app.renderer.start(); CHECK(idle(app));
    ui.tick(app); ui.tick(app); app.nodePositions = incoming.nodePositions;
    app.graphCanvas.framePending = false; app.graphCanvas.view = {{20, 40}, 1}; ui.tick(app);
    CHECK(selectNode(app, app.primarySource)); // Either menu must select its owner.
    auto &io = ImGui::GetIO(); io.ConfigMacOSXBehaviors = mac;
    // ImGui maps submitted macOS Ctrl to Super and aliases Super+left to right.
    // Keep the modifier held across frames: it must not dismiss either menu.
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    const auto target = ui.screen(app, inputMenu ? PersistPosition{320, 44} : PersistPosition{370, 16});
    if (mac) ui.click(app, target); else rightClick(ui, app, target);
    ui.tick(app);
    CHECK(app.selectedNodeId() == reused && ui.context->OpenPopupStack.Size == 1);
    CHECK(inputMenu ? app.graphCanvas.contextPort && app.graphCanvas.contextPort->nodeId == reused
                    : app.graphCanvas.contextNode == reused);
    const auto action = lastPopupItem(); CHECK(action.x > 0 && action.y > 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, false); ui.tick(app);
    CHECK(ui.context->OpenPopupStack.Size == 1);
    std::atomic<int> mutations{0}; RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
    CHECK(pasteFromClipboard(app) && idle(app)); // Same photo, different logical node at the same ID.
    CHECK(app.path == "same-photo" && !app.selectedNodeId() && mutations == 1);
    CHECK(findNode(app, reused) && findNode(app, reused)->processor->identifier() == "rawnode.native.cst");
    CHECK(app.nodes.size() == 2 && consumes(app, reused, app.primarySource) && app.documentOutput == imageOf(reused));
    const auto expected = graphJson(incoming);
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(graphJson(app) == expected && samePositions(app.nodePositions, incoming.nodePositions));
    ui.click(app, action); CHECK(idle(app)); // Try the old Delete Node / Disconnect action.
    CHECK(graphJson(app) == expected && samePositions(app.nodePositions, incoming.nodePositions));
    CHECK(app.nodes.size() == 2 && consumes(app, reused, app.primarySource) && app.documentOutput == imageOf(reused));
    CHECK(ui.context->OpenPopupStack.Size == 0 && mutations == 1);
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(after.epoch == before.epoch && after.mutationDepth == 0);
    CHECK(after.previewPending == before.previewPending && after.quietPending == before.quietPending &&
          after.displayPending == before.displayPending && !after.previewBusy);
    RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  }
  return true;
}

bool editedWirePhotoPersistence() {
  Files files; const auto first = (files.dir / "wired.png").string(), second = (files.dir / "other.png").string();
  std::ofstream(first) << "synthetic"; std::ofstream(second) << "synthetic";
  document_detail::ActionTestOptions options; options.decode = syntheticPhoto;
  App app; CHECK(document_detail::openPhoto(app, first, {}, options));
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId();
  Ui ui; app.nodesView = NodesView::Graph; ui.tick(app); ui.tick(app);
  app.nodePositions = {{app.primarySource.value, {-10.25, 0}}, {a.value, {320.5, 0}}, {b.value, {640, -0.5}}};
  app.graphCanvas.framePending = false; app.graphCanvas.view = {{20, 40}, 0.6}; ui.tick(app);
  const auto positions = app.nodePositions;
  const auto source = ui.screen(app, {229.75, 44}), input = ui.screen(app, {640, 43.5});
  ui.drag(app, source, {input.x - source.x, input.y - source.y}, 0);
  const auto output = ui::layoutGraph(graph(app), app.nodePositions).output;
  const auto outputPort = ui.screen(app, {output.x, output.y + 48});
  ui.drag(app, source, {outputPort.x - source.x, outputPort.y - source.y}, 0);
  CHECK(consumes(app, a, app.primarySource) && consumes(app, b, app.primarySource));
  CHECK(app.documentOutput == imageOf(app.primarySource) && samePositions(app.nodePositions, positions));
  CHECK(saveCurrentInputSidecar(app));
  PersistFile saved; std::string error;
  CHECK(decodeGraphFile(bytes(inputSidecarPath(first)), saved, error));
  CHECK(saved.grade && saved.grade->graph.nodes.size() == 3);
  CHECK(saved.grade->graph.documentOutput == imageOf(app.primarySource) && samePositions(saved.grade->positions, positions));
  const auto expected = graphJson(app);
  // Actual photo operations save/reconstruct the authoritative topology. A wire
  // held across photo switching is cancelled by UI lifecycle, not GraphCommands.
  ui.hover(app, source); ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app); CHECK(app.graphCanvas.wire);
  CHECK(document_detail::openPhoto(app, second, {}, options)); ui.tick(app); CHECK(!app.graphCanvas.wire);
  ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app);
  CHECK(document_detail::openPhoto(app, first, {}, options)); ui.tick(app);
  CHECK(graphJson(app) == expected && samePositions(app.nodePositions, positions));
  CHECK(consumes(app, b, app.primarySource) && app.documentOutput == imageOf(app.primarySource));
  return true;
}

}  // namespace

bool testGraphView() {
  for (const auto &test : {
       std::make_pair("dependency layout, branches, missing nodes and typed ports", dependencyLayoutAndPorts),
       std::make_pair("wire editing, no-op/cancel, Disconnect and port priority at zoom limits", wireEditingFrames),
       std::make_pair("typed Output wiring, pixel results and UI export/protection gates", outputAndTypedWireFrames),
       std::make_pair("focused nonrepeat deletion, Inspector cleanup and off-spine menu deletion", deletionFrames),
       std::make_pair("List Delete/Backspace reuse splice, selection cleanup and render behaviour", listDeletionFrames),
       std::make_pair("List deletion focus, endpoints, protection, export, modifiers and held keys", listDeletionSafetyFrames),
       std::make_pair("List post-deletion highlight, neutral Inspector and explicit reselection", listPostDeletionHighlightFrames),
       std::make_pair("List deletion refusal, disconnected consumers, view switches and X button", listDeletionRulesAndViewSwitching),
       std::make_pair("UI-only cancellation for removed endpoints, modifiers, views and photos", gestureEndpointLifecycle),
       std::make_pair("node/input menus close after same-photo Full Grade Paste reuses IDs, including macOS Ctrl+click", contextMenusAfterGradePaste),
       std::make_pair("edited wire topology and fractional positions save, switch and reopen", editedWirePhotoPersistence),
       std::make_pair("coordinate inverses, cursor zoom and frame bounds", coordinateTransforms),
       std::make_pair("real canvas events, selection and render-demand isolation", canvasInteraction),
       std::make_pair("node hit testing, graph-space dragging and cursor zoom at 0.5x/2x", zoomedNodeInteraction),
       std::make_pair("Frame All focus, active text input, other panel and view switching", frameShortcutFocus),
       std::make_pair("photo layout save, failure, codec and reopen lifecycle", photoLayoutLifecycle),
       std::make_pair("contextual Inspector, List endpoints, protected controls and selection isolation", inspectorContexts),
       std::make_pair("Inspector RAW gamma edits re-decode/render the photo and preserve global defaults", inspectorRawGammaEdit),
       std::make_pair("searchable processor picker inserts by shared selection in List and Graph", processorPickerInBothViews),
       std::make_pair("new workspace default and custom/floating ini restart", workspaceDocking),
       std::make_pair("first-launch framing waits for settled docking geometry", firstDockedFrame)}) {
    if (!test.second()) { std::fprintf(stderr, "FAILED graph view: %s\n", test.first); return false; }
    std::printf("ok  Graph View: %s\n", test.first);
  }
  return true;
}
