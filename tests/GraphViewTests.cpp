#include "GraphViewTests.h"

#include "AppState.h"
#include "GraphCommands.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/DockLayout.h"
#include "ui/GraphLayout.h"
#include "ui/UiContext.h"

#include "imgui.h"
#include "imgui_internal.h"

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
  CHECK(addNativeCstNode(app));
  for (auto view : {NodesView::List, NodesView::Graph}) {
    app.nodesView = view; CHECK(selectNode(app, app.primarySource));
    const auto previousOutput = app.documentOutput;
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
    CHECK(node->fixedInputs.at(ports::image) == previousOutput);
    CHECK(app.documentOutput == (OutputRef{node->id, ports::image}) && app.pluginFilter[0] == '\0');
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

}  // namespace

bool testGraphView() {
  for (const auto &test : {
       std::make_pair("dependency layout, branches, missing nodes and typed ports", dependencyLayoutAndPorts),
       std::make_pair("coordinate inverses, cursor zoom and frame bounds", coordinateTransforms),
       std::make_pair("real canvas events, selection and render-demand isolation", canvasInteraction),
       std::make_pair("node hit testing, graph-space dragging and cursor zoom at 0.5x/2x", zoomedNodeInteraction),
       std::make_pair("Frame All focus, active text input, other panel and view switching", frameShortcutFocus),
       std::make_pair("photo layout save, failure, codec and reopen lifecycle", photoLayoutLifecycle),
       std::make_pair("contextual Inspector, List endpoints, protected controls and selection isolation", inspectorContexts),
       std::make_pair("searchable processor picker appends in List and Graph", processorPickerInBothViews),
       std::make_pair("new workspace default and custom/floating ini restart", workspaceDocking),
       std::make_pair("first-launch framing waits for settled docking geometry", firstDockedFrame)}) {
    if (!test.second()) { std::fprintf(stderr, "FAILED graph view: %s\n", test.first); return false; }
    std::printf("ok  Graph View: %s\n", test.first);
  }
  return true;
}
