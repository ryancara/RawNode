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
    ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({1150, 680});
    ImGui::Begin("Nodes test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto start = ImGui::GetCursorScreenPos(); const auto &style = ImGui::GetStyle();
    listButton = {start.x + 8, start.y + 8};
    graphButton = {start.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("List").x +
                     2 * style.ItemSpacing.x + ImGui::CalcTextSize("|").x + 8, start.y + 8};
    frameButton = {start.x + 20, start.y + ImGui::GetFrameHeight() + style.ItemSpacing.y +
                   style.ItemSpacing.y + 1 + 8};
    drawNodesPanel(app);
    ImGui::End(); ImGui::EndFrame();
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
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId;
  CHECK(addNativeExposureNode(app)); const auto b = *app.selectedNodeId;
  app.renderer.start(); CHECK(idle(app));
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  const auto topology = graphJson(app);
  std::atomic<int> mutations{0};
  RenderRuntimeTestAccess::observe(app.renderer, &mutations, observeMutation);
  ui.tick(app); ui.tick(app);
  ui.click(app, ui.graphButton);
  CHECK(app.nodesView == NodesView::Graph && app.selectedNodeId == b);
  CHECK(app.nodePositions.size() == 3); // Source and both processors, no virtual Output.
  app.graphCanvas.view = {{30, 30}, 1}; app.graphCanvas.framePending = false; ui.tick(app);
  const auto original = app.nodePositions.at(a.value);
  app.paramFilter[0] = 'x';
  ui.click(app, ui.screen(app, {original.x + 70, original.y + 16}));
  CHECK(app.selectedNodeId == a && app.paramFilter[0] == '\0');
  ui.drag(app, ui.screen(app, {original.x + 70, original.y + 16}), {45, 55}, 0);
  CHECK(same(app.nodePositions.at(a.value), {original.x + 45, original.y + 55}));
  const auto moved = app.nodePositions.at(a.value);
  // Body clicks select, but cannot reposition. Source headers can reposition.
  ui.drag(app, ui.screen(app, {moved.x + 70, moved.y + 45}), {20, 25}, 0);
  CHECK(same(app.nodePositions.at(a.value), moved));
  const auto source = app.nodePositions.at(app.primarySource.value);
  ui.drag(app, ui.screen(app, {source.x + 50, source.y + 16}), {12, 18}, 0);
  CHECK(same(app.nodePositions.at(app.primarySource.value), {source.x + 12, source.y + 18}));
  const auto pan = app.graphCanvas.view.pan;
  const ImVec2 blank{ui.canvasOrigin.x + 600, ui.canvasOrigin.y + 300};
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
  app.paramFilter[0] = 'e';
  ui.click(app, ui.listButton);
  CHECK(app.nodesView == NodesView::List && app.selectedNodeId == a && app.paramFilter[0] == 'e');
  const auto listPan = app.graphCanvas.view.pan;
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F, true); ui.tick(app);
  ImGui::GetIO().AddKeyEvent(ImGuiKey_F, false); ui.tick(app);
  CHECK(same(app.graphCanvas.view.pan, listPan));
  ui.click(app, ui.graphButton);
  CHECK(app.selectedNodeId == a && app.paramFilter[0] == 'e' && same(app.nodePositions.at(a.value), moved));
  // Toolbar framing works after a viewport change, without moving document data.
  app.graphCanvas.view.pan = {900, 900}; ui.tick(app);
  ui.click(app, ui.frameButton);
  CHECK(!same(app.graphCanvas.view.pan, {900, 900}));
  // Protected photos still allow viewing, selection and navigation, not layout edits.
  app.sidecarWriteBlockedPath = app.path;
  CHECK(!ui::moveGraphNode(app, a, {100, 100}));
  ui.drag(app, ui.screen(app, {moved.x + 70, moved.y + 16}), {50, 50}, 0);
  CHECK(same(app.nodePositions.at(a.value), moved));
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
  CHECK(addNativeExposureNode(app)); const auto id = *app.selectedNodeId;
  CHECK(ui::moveGraphNode(app, id, {-42.5, 19.25}));
  CHECK(ui::moveGraphNode(app, app.primarySource, {-300, 10}));
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
  CHECK(saved.grade->graph.nodes.size() == 2 && saved.grade->graph.documentOutput.nodeId == id);
  // The normal codec and reopen path preserve exactly the stable-ID layout.
  std::string json; PersistFile decoded;
  CHECK(encodeGraphFile(saved, json, error) && decodeGraphFile(json, decoded, error));
  CHECK(same(decoded.grade->positions.at(id.value), {-64.5, 80.125}));
  CHECK(json.find("zoom") == std::string::npos && json.find("pan") == std::string::npos);
  CHECK(document_detail::openPhoto(app, a, {}, options));
  CHECK(findNode(app, id) && same(app.nodePositions.at(id.value), {-64.5, 80.125}));
  Ui ui; app.nodesView = NodesView::Graph; ui.tick(app); ui.tick(app);
  app.graphCanvas.view.pan = {999, 999};
  CHECK(document_detail::openPhoto(app, b, {}, options)); ui.tick(app);
  CHECK(app.graphCanvas.photoPath == b && !same(app.graphCanvas.view.pan, {999, 999}));
  CHECK(!app.selectedNodeId && app.nodesView == NodesView::Graph && app.nodePositions.size() == 1);
  app.sidecarWriteBlockedPath = app.path; app.nodePositions.clear(); ui.tick(app);
  CHECK(app.nodePositions.empty()); // Viewing a protected graph cannot materialize layout.
  app.sidecarWriteBlockedPath.clear();
  CHECK(document_detail::openPhoto(app, a, {}, options)); ui.tick(app);
  CHECK(same(app.nodePositions.at(id.value), {-64.5, 80.125}));
  return true;
}

bool dockingCompatibility() {
  for (const bool savedFloatingNodes : {false, true}) {
    Ui ui;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (savedFloatingNodes)
      ImGui::LoadIniSettingsFromMemory("[Window][Nodes]\nPos=100,100\nSize=600,250\nCollapsed=0\n\n");
    ImGui::GetIO().DisplaySize = {1280, 800}; ImGui::GetIO().DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    const ImGuiID root = ImHashStr("OfxRawHostDock", 0, ImHashStr("##DockHost"));
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, {1280, 800});
    ImGuiID center = root, left = 0, right = 0, strip = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.29f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.17f, &strip, &center);
    ImGui::DockBuilderDockWindow(DockLayout::kLeft, left);
    ImGui::DockBuilderDockWindow(DockLayout::kParams, right);
    ImGui::DockBuilderDockWindow(DockLayout::kFilmstrip, strip);
    ImGui::DockBuilderDockWindow(DockLayout::kPreview, center);
    ImGui::DockBuilderFinish(root);
    const auto leftSize = ImGui::DockBuilderGetNode(left)->Size;
    const auto rightSize = ImGui::DockBuilderGetNode(right)->Size;
    const auto stripSize = ImGui::DockBuilderGetNode(strip)->Size;
    App app;
    DockLayout::BeginMainDockSpace(app);
    const auto settings = [](const char *name) { return ImGui::FindWindowSettingsByID(ImHashStr(name)); };
    CHECK(settings(DockLayout::kLeft)->DockId == left && settings(DockLayout::kParams)->DockId == right);
    CHECK(settings(DockLayout::kFilmstrip)->DockId == strip);
    CHECK(ImGui::DockBuilderGetNode(left)->Size.x == leftSize.x);
    CHECK(ImGui::DockBuilderGetNode(right)->Size.x == rightSize.x);
    CHECK(ImGui::DockBuilderGetNode(strip)->Size.y == stripSize.y);
    const auto *nodes = settings(DockLayout::kNodes);
    CHECK(nodes);
    if (savedFloatingNodes) {
      CHECK(nodes->DockId == 0 && settings(DockLayout::kPreview)->DockId == center);
    } else {
      const auto *nodeDock = ImGui::DockBuilderGetNode(nodes->DockId);
      const auto *previewDock = ImGui::DockBuilderGetNode(settings(DockLayout::kPreview)->DockId);
      CHECK(nodeDock && previewDock && nodeDock->ParentNode == previewDock->ParentNode);
      CHECK(nodeDock->Pos.y > previewDock->Pos.y);
      const auto nodesId = nodes->DockId, previewId = settings(DockLayout::kPreview)->DockId;
      DockLayout::BeginMainDockSpace(app);
      CHECK(settings(DockLayout::kNodes)->DockId == nodesId && settings(DockLayout::kPreview)->DockId == previewId);
    }
    ImGui::EndFrame();
  }
  // With no saved tree, the default Nodes dock is below Preview.
  Ui ui; ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ImGui::GetIO().DisplaySize = {1280, 800}; ImGui::GetIO().DeltaTime = 1.0f / 60;
  ImGui::NewFrame(); App app; DockLayout::BeginMainDockSpace(app);
  const auto dock = [](const char *name) {
    return ImGui::DockBuilderGetNode(ImGui::FindWindowSettingsByID(ImHashStr(name))->DockId);
  };
  CHECK(dock(DockLayout::kNodes)->Pos.y > dock(DockLayout::kPreview)->Pos.y);
  CHECK(dock(DockLayout::kFilmstrip)->Pos.y > dock(DockLayout::kNodes)->Pos.y);
  CHECK(dock(DockLayout::kParams)->Pos.x > dock(DockLayout::kNodes)->Pos.x);
  ImGui::EndFrame();
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
       std::make_pair("photo layout save, failure, codec and reopen lifecycle", photoLayoutLifecycle),
       std::make_pair("default docking and saved/custom/floating layout compatibility", dockingCompatibility),
       std::make_pair("first-launch framing waits for settled docking geometry", firstDockedFrame)}) {
    if (!test.second()) { std::fprintf(stderr, "FAILED graph view: %s\n", test.first); return false; }
    std::printf("ok  Graph View: %s\n", test.first);
  }
  return true;
}
