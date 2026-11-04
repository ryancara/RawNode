#include "PreviewInteractionTests.h"

#include "AppState.h"
#include "Export.h"
#include "GraphCommands.h"
#include "processors/OfxProcessor.h"
#include "processors/CtlProcessor.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActions.h"
#include "ui/ParamWidgets.h"
#include "ui/UiContext.h"
#include "imgui_internal.h"

#include <chrono>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <condition_variable>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "Preview interaction check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
using ui::PreviewDrag;
bool near(double a, double b) { return std::abs(a - b) < 2e-6; } // Screen float rounding, below 0.002 px here.
double parameter(const App &app, const NodeId &id, const char *name) {
  for (const auto &p : findNode(app, id)->processor->parameters())
    if (p.id == name) return std::get<double>(p.value);
  return -99;
}
bool fixture(App &app) {
  app.path = "preview-handles.png";
  app.full.w = 32; app.full.h = 16;
  for (int i = 0; i < 32 * 16; ++i) app.full.px.insert(app.full.px.end(), {.25f, -.5f, 2, .375f});
  app.preview = std::make_shared<Image>(app.full);
  CHECK(addNativeRadialMaskNode(app));
  const auto radial = *app.selectedNodeId();
  selectOutput(app); CHECK(addNativeExposureNode(app));
  CHECK(graph::connect(app, {radial,ports::mask}, {*app.selectedNodeId(),std::nullopt,ports::mask}).ok());
  selectNode(app,radial);
  app.history.reset(app);
  return true;
}
ui::RadialGeometry geometry(const App &app) {
  const auto id = *app.selectedNodeId();
  return {parameter(app, id, "centreX"), parameter(app, id, "centreY"), parameter(app, id, "radiusX"),
          parameter(app, id, "radiusY"), parameter(app, id, "feather")};
}

// Retain actual evaluator publications. Only the GL upload is replaced here;
// its texture identity is acknowledged exactly as pumpDisplayUpload does.
void fakeUpload(App &app, bool onlyIfDirty = false) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  if (onlyIfDirty && !app.displayDirty) return;
  app.texW = app.display.w; app.texH = app.display.h;
  app.tex = app.texW > 0 && app.texH > 0 ? 1 : 0;
  app.textureGeneration = app.processed.generation;
  app.textureRequest = app.processed.request;
  app.displayDirty = false;
}
void publishFrame(App &app, std::optional<CoordinateMapping> mappingOverride = std::nullopt) {
  const auto mode = previewMode(app);
  const OutputRef endpoint = mode == PreviewMode::MaskOnly ? *selectedMaskOutput(app) : app.documentOutput;
  const EvaluationContext context{EvaluationPurpose::Preview,app.full.w,app.full.h,app.preview->w,app.preview->h};
  auto result = evaluateGraph({app.nodes,app.primarySource,app.documentOutput},endpoint,context,app.preview);
  std::optional<EvaluationResult> overlayMask;
  if (mode == PreviewMode::MaskOverlay) {
    auto pair = evaluateGraphPair({app.nodes,app.primarySource,app.documentOutput},endpoint,
                                 *selectedMaskOutput(app),context,app.preview);
    result = std::move(pair.first); overlayMask = std::move(pair.second);
  }
  if (mappingOverride)
    for (auto &placement : result.maskPlacements) placement.maskToImage = *mappingOverride;
  auto presentation = display_detail::preparePresentation(app,result,overlayMask);
  display_detail::publishPreview(app,std::move(result),RenderRuntimeTestAccess::snapshot(app.renderer).epoch,
      app.renderer.previewDemand(),std::make_shared<OutputRef>(endpoint),std::move(presentation),std::move(overlayMask));
  fakeUpload(app);
}

// Real Preview and ImGui input, without a GL context. Only texture upload is
// omitted: the fake texture supplies the dimensions of the displayed raster.
struct Ui {
  ImGuiContext *previous = ImGui::GetCurrentContext(), *context = ImGui::CreateContext();
  ImRect canvas;
  ui::PreviewTransform view;
  bool visible = true, otherControls = false, texture = true, autoPublish = true, autoUpload = true;
  ImVec2 textPoint;
  std::optional<CoordinateMapping> mappingOverride; // Synthetic published geometry for affine-only UI tests.
  char text[32] = {};
  Ui() {
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Production leaves keyboard navigation disabled.
    unsigned char *pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  }
  ~Ui() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
  void tick(App &app) {
    if (autoPublish) publishFrame(app, mappingOverride);
    if (autoUpload) fakeUpload(app, true); // Production consumes pending uploads before the UI frame.
    auto &io = ImGui::GetIO(); io.DisplaySize = {1400, 900}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    if (visible) {
      if (!texture) app.tex = 0;
      ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({912, 650});
      ImGui::Begin("Preview test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      if (autoUpload) drawPreviewPanel(app,[](App &app) { fakeUpload(app,true); });
      else drawPreviewPanel(app,[](App &) {}); // Synthetic publication after the upload phase.
      canvas = context->LastItemData.Rect;
      const float w = float(std::max(app.texW,1)), h = float(std::max(app.texH,1));
      const float fit = std::min(canvas.GetWidth() / w, canvas.GetHeight() / h);
      const ImVec2 size{w * fit * app.previewZoom, h * fit * app.previewZoom};
      view = {{canvas.GetCenter().x + app.previewPanX - size.x / 2,
               canvas.GetCenter().y + app.previewPanY - size.y / 2}, size};
      ImGui::End();
    }
    if (otherControls) {
      ImGui::SetNextWindowPos({950, 20}); ImGui::SetNextWindowSize({400, 250});
      ImGui::Begin("Other controls");
      ImGui::InputText("Text", text, sizeof text);
      textPoint = ImGui::GetItemRectMin(); textPoint.x += 10; textPoint.y += 8;
      ImGui::Button("Button"); ImGui::End();
    }
    ui::finishPreviewInteraction(app);
    ImGui::EndFrame();
  }
  void hover(App &app, ImVec2 point) { ImGui::GetIO().AddMousePosEvent(point.x, point.y); tick(app); }
  void press(App &app, ImVec2 point, int button = 0) {
    hover(app, point); ImGui::GetIO().AddMouseButtonEvent(button, true); tick(app);
  }
  void release(App &app, int button = 0) { ImGui::GetIO().AddMouseButtonEvent(button, false); tick(app); }
  void key(App &app, ImGuiKey key, bool down) { ImGui::GetIO().AddKeyEvent(key, down); tick(app); }
  ImVec2 grip(App &app, PreviewDrag kind) {
    if (autoPublish && ui::radialPlacement(app).state == ui::PlacementState::Pending) tick(app);
    for (const auto &h : ui::radialHandles(view, geometry(app), ui::radialPlacement(app).mapping)) if (h.kind == kind) return h.grip;
    return {};
  }
  void drag(App &app, PreviewDrag kind, ImVec2 delta) {
    const auto start = grip(app, kind); press(app, start);
    hover(app, {start.x + delta.x, start.y + delta.y}); release(app);
  }
};

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("rawnode-preview-handles-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { std::filesystem::create_directory(dir); }
  ~Files() { std::error_code ignored; std::filesystem::remove_all(dir, ignored); }
  std::string path(const char *name) const { return (dir / name).string(); }
};

bool mappingsAndOverlap() {
  const ui::PreviewTransform view{{100, 200}, {800, 400}};
  const auto bottom = view.toScreen(0, 0), top = view.toScreen(1, 1), point = view.toScreen(.25, .75);
  CHECK(bottom.x == 100 && bottom.y == 600 && top.x == 900 && top.y == 200);
  CHECK(point.x == 300 && point.y == 300);
  CHECK(view.toImage({300, 300}) == (std::array<double, 2>{.25, .75}));
  CHECK(view.toImage({-300, 800}) == (std::array<double, 2>{-.5, -.5}));
  // A 2x zoom plus pan of (+30,-20), around the original photo centre.
  const ui::PreviewTransform zoomed{{-270, -20}, {1600, 800}};
  const auto moved = zoomed.toScreen(.25, .75);
  CHECK(moved.x == 130 && moved.y == 180);
  CHECK(zoomed.toImage(moved) == (std::array<double, 2>{.25, .75}));
  for (auto size : {ImVec2{800, 400}, ImVec2{200, 800}, ImVec2{8, 4}}) {
    const ui::PreviewTransform scaled{{100, 200}, size};
    const auto p = scaled.toScreen(.125, .875);
    const auto normal = scaled.toImage(p);
    CHECK(near(normal[0], .125) && near(normal[1], .875));
    for (double rx : {0.0, .001, .25}) for (double ry : {0.0, .001, .25})
        for (double feather : {0.0, .5, 1.0}) {
      const auto handles = ui::radialHandles(scaled, {.5, .5, rx, ry, feather});
      for (const auto &h : handles) CHECK(ui::hitRadialHandle(handles, h.grip) == h.kind);
      const ImVec2 centre{100 + size.x / 2, 200 + size.y / 2};
      CHECK(handles[0].anchor.x == centre.x && handles[0].anchor.y == centre.y);
      CHECK(handles[0].grip.x == centre.x && handles[0].grip.y == centre.y);
      CHECK(handles[1].grip.x < centre.x && handles[1].grip.y == centre.y);
      CHECK(handles[2].grip.x > centre.x && handles[2].grip.y == centre.y);
      CHECK(handles[3].grip.y < centre.y && handles[3].grip.x == centre.x);
      CHECK(handles[4].grip.y > centre.y && handles[4].grip.x == centre.x);
      CHECK(handles[5].grip.x >= centre.x && handles[5].grip.y <= centre.y);
      // Exact geometry stays on its original ellipse, including zero axes.
      CHECK(std::abs(handles[1].anchor.x - (centre.x - rx * size.x)) < 1e-4 && handles[1].anchor.y == centre.y);
      CHECK(std::abs(handles[2].anchor.x - (centre.x + rx * size.x)) < 1e-4 && handles[2].anchor.y == centre.y);
      CHECK(std::abs(handles[3].anchor.y - (centre.y - ry * size.y)) < 1e-4 && handles[3].anchor.x == centre.x);
      CHECK(std::abs(handles[4].anchor.y - (centre.y + ry * size.y)) < 1e-4 && handles[4].anchor.x == centre.x);
      const auto &featherHandle = handles[5];
      CHECK(std::abs(featherHandle.anchor.x - (centre.x + size.x * rx * (1 - feather) / std::sqrt(2.0))) < 1e-4);
      CHECK(std::abs(featherHandle.anchor.y - (centre.y - size.y * ry * (1 - feather) / std::sqrt(2.0))) < 1e-4);
      if (featherHandle.grip.x != featherHandle.anchor.x || featherHandle.grip.y != featherHandle.anchor.y) {
        CHECK(featherHandle.grip.x > featherHandle.anchor.x && featherHandle.grip.y < featherHandle.anchor.y);
        CHECK(std::abs((featherHandle.grip.x - featherHandle.anchor.x) -
                       (featherHandle.anchor.y - featherHandle.grip.y)) < 1e-4);
      }
    }
  }
  return true;
}

bool collapsedRadiusRecovery() {
  for (auto kind : {PreviewDrag::Left, PreviewDrag::Right, PreviewDrag::Top, PreviewDrag::Bottom}) {
    App app; Ui ui; CHECK(fixture(app));
    const auto id = *app.selectedNodeId();
    CHECK(setNodeParameterValue(app, id, "radiusX", 0.0) && setNodeParameterValue(app, id, "radiusY", 0.0));
    CHECK(app.history.recordCompletedEdit(app));
    ui.tick(app); ui.tick(app);
    const auto before = app.history.size(), cursor = app.history.cursor();
    const auto start = ui.grip(app, kind), size = ui.view.size;
    const bool horizontal = kind == PreviewDrag::Left || kind == PreviewDrag::Right;
    const ImVec2 delta = kind == PreviewDrag::Left ? ImVec2{-32, 0} :
                         kind == PreviewDrag::Right ? ImVec2{32, 0} :
                         kind == PreviewDrag::Top ? ImVec2{0, -32} : ImVec2{0, 32};
    ui.press(app, start);
    CHECK(app.previewInteraction.drag == kind);
    CHECK(parameter(app, id, "radiusX") == 0 && parameter(app, id, "radiusY") == 0);
    ui.hover(app, {start.x + delta.x, start.y + delta.y});
    CHECK(near(parameter(app, id, horizontal ? "radiusX" : "radiusY"), 32 / (horizontal ? size.x : size.y)));
    CHECK(parameter(app, id, horizontal ? "radiusY" : "radiusX") == 0 && app.history.size() == before);
    ui.release(app);
    CHECK(app.history.size() == before + 1 && app.history.cursor() == cursor + 1);
    CHECK(parameter(app, id, "centreX") == .5 && parameter(app, id, "centreY") == .5);
  }
  return true;
}

bool editsAndHistory() {
  App app; Ui ui; CHECK(fixture(app)); ui.tick(app); ui.tick(app);
  const auto id = *app.selectedNodeId();
  const auto before = app.history.size();
  const auto start = ui.grip(app, PreviewDrag::Centre), size = ui.view.size;
  ui.press(app, start);
  for (int i = 1; i <= 8; ++i) {
    const auto epoch = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
    ui.hover(app, {start.x + i * 10, start.y - i * 5});
    CHECK(app.history.size() == before && !canUndoGrade(app));
    CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch > epoch);
  }
  CHECK(near(parameter(app, id, "centreX"), .5 + 80 / size.x));
  CHECK(near(parameter(app, id, "centreY"), .5 + 40 / size.y));
  CHECK(app.previewPanX == 0 && app.previewPanY == 0);
  ui.release(app);
  CHECK(app.history.size() == before + 1 && !app.previewInteraction.editPending);
  const auto final = geometry(app);
  CHECK(app.history.undo(app) && parameter(app, id, "centreX") == .5 && parameter(app, id, "centreY") == .5);
  CHECK(app.history.redo(app) && parameter(app, id, "centreX") == final.centreX && parameter(app, id, "centreY") == final.centreY);
  ui.tick(app);
  auto count = app.history.size(); ui.press(app, ui.grip(app, PreviewDrag::Centre)); ui.release(app);
  CHECK(app.history.size() == count);
  const auto returnedStart = ui.grip(app, PreviewDrag::Centre);
  const auto cursor = app.history.cursor();
  ui.press(app, returnedStart); ui.hover(app, {returnedStart.x + 32, returnedStart.y - 24});
  CHECK(parameter(app, id, "centreX") != final.centreX && parameter(app, id, "centreY") != final.centreY);
  ui.hover(app, returnedStart); ui.release(app);
  CHECK(parameter(app, id, "centreX") == final.centreX && parameter(app, id, "centreY") == final.centreY);
  CHECK(app.history.size() == count && app.history.cursor() == cursor && !app.history.hasUnrecordedEdit());
  for (const auto kind : {PreviewDrag::Right, PreviewDrag::Left, PreviewDrag::Top, PreviewDrag::Bottom}) {
    const bool horizontal = kind == PreviewDrag::Right || kind == PreviewDrag::Left;
    const char *name = horizontal ? "radiusX" : "radiusY";
    const double previous = parameter(app, id, name);
    const ImVec2 delta = kind == PreviewDrag::Right ? ImVec2{30, 0} :
                         kind == PreviewDrag::Left ? ImVec2{-30, 0} :
                         kind == PreviewDrag::Top ? ImVec2{0, -30} : ImVec2{0, 30};
    count = app.history.size(); ui.drag(app, kind, delta);
    CHECK(near(parameter(app, id, name), previous + 30 / (horizontal ? size.x : size.y)));
    CHECK(app.history.size() == count + 1 && parameter(app, id, "centreX") == final.centreX);
  }
  // Move one quarter of the diagonal spoke inward: f=.5 -> .75.
  const auto g = geometry(app);
  ui.drag(app, PreviewDrag::Feather, {float(-.25 * g.radiusX * size.x / std::sqrt(2.0)),
                                     float(.25 * g.radiusY * size.y / std::sqrt(2.0))});
  // ImGui floors fractional mouse positions to screen pixels. At most one
  // pixel per axis gives this geometric bound on the diagonal projection.
  CHECK(std::abs(parameter(app, id, "feather") - .75) < 2 / std::hypot(g.radiusX * size.x, g.radiusY * size.y));
  ui.drag(app, PreviewDrag::Feather, {4000, -4000}); CHECK(parameter(app, id, "feather") == 0);
  ui.drag(app, PreviewDrag::Feather, {-4000, 4000}); CHECK(parameter(app, id, "feather") == 1);
  ui.drag(app, PreviewDrag::Right, {-4000, 0}); CHECK(parameter(app, id, "radiusX") == 0);
  ui.drag(app, PreviewDrag::Top, {0, 4000}); CHECK(parameter(app, id, "radiusY") == 0);
  // Individually reachable displaced grips recover both zero radii without a jump.
  ui.drag(app, PreviewDrag::Right, {40, 0}); CHECK(near(parameter(app, id, "radiusX"), 40 / size.x));
  ui.drag(app, PreviewDrag::Top, {0, -40}); CHECK(near(parameter(app, id, "radiusY"), 40 / size.y));
  ui.drag(app, PreviewDrag::Right, {4000, 0}); CHECK(parameter(app, id, "radiusX") == 2);
  ui.drag(app, PreviewDrag::Centre, {-4000, 4000});
  CHECK(parameter(app, id, "centreX") == -1 && parameter(app, id, "centreY") == -1);
  return true;
}

bool ownershipAndZoom() {
  App app; Ui ui; CHECK(fixture(app)); ui.tick(app); ui.tick(app);
  const ImVec2 empty{ui.canvas.Min.x + 30, ui.canvas.Min.y + 30};
  ui.press(app, empty); ui.key(app, ImGuiKey_Space, true); ui.hover(app, {empty.x + 40, empty.y + 30});
  CHECK(app.previewPanX == 0 && app.previewPanY == 0 && app.previewInteraction.drag == PreviewDrag::Ignore);
  ui.release(app); ui.key(app, ImGuiKey_Space, false);
  ui.key(app, ImGuiKey_Space, true); ui.press(app, empty);
  ui.key(app, ImGuiKey_Space, false); ui.hover(app, {empty.x + 40, empty.y + 30});
  CHECK(app.previewPanX == 40 && app.previewPanY == 30 && app.previewInteraction.drag == PreviewDrag::Pan);
  ui.release(app);
  ui.press(app, empty, 2); ui.hover(app, {empty.x - 10, empty.y + 20}); ui.release(app, 2);
  CHECK(app.previewPanX == 30 && app.previewPanY == 50);
  const auto centre = ui.grip(app, PreviewDrag::Centre);
  ui.press(app, centre); ui.key(app, ImGuiKey_Space, true); ui.hover(app, {centre.x + 20, centre.y});
  CHECK(app.previewInteraction.drag == PreviewDrag::Centre && app.previewPanX == 30);
  ui.release(app); ui.key(app, ImGuiKey_Space, false);
  app.previewZoom = 2; app.previewPanX = 37; app.previewPanY = -19; ui.tick(app);
  const auto point = ui.view.toScreen(.25, .75);
  ui.hover(app, point); ImGui::GetIO().AddMouseWheelEvent(0, 1); ui.tick(app);
  CHECK(near(app.previewZoom, 2.2));
  const auto anchored = ui.view.toImage(point);
  CHECK(near(anchored[0], .25) && near(anchored[1], .75));
  const auto size = ui.view.size;
  const auto id = *app.selectedNodeId(); const auto x = parameter(app, id, "centreX");
  ui.drag(app, PreviewDrag::Centre, {40, -20});
  CHECK(near(parameter(app, id, "centreX"), x + 40 / size.x));
  CHECK(app.previewZoom > 2);
  auto dragStart = ui.grip(app, PreviewDrag::Centre); ui.press(app, dragStart);
  ui.hover(app, {dragStart.x + 30, dragStart.y - 20});
  const auto beforeZoom = parameter(app, id, "centreX");
  ImGui::GetIO().AddMouseWheelEvent(0, 1); ui.tick(app); ui.tick(app);
  CHECK(near(parameter(app, id, "centreX"), beforeZoom));
  ui.release(app);
  ui.press(app, empty); ui.release(app); ui.press(app, empty); ui.release(app);
  CHECK(app.previewZoom == 1 && app.previewPanX == 0 && app.previewPanY == 0);
  // A held Space while editing text must not capture a trackpad left-drag.
  ui.otherControls = true; ui.tick(app); ui.tick(app);
  ui.press(app, ui.textPoint); ui.release(app); ui.key(app, ImGuiKey_Space, true);
  ui.press(app, empty); ui.hover(app, {empty.x + 25, empty.y + 25}); ui.release(app);
  CHECK(app.previewPanX == 0 && app.previewPanY == 0);
  ui.key(app, ImGuiKey_Space, false);
  // Native Mac keyboard behaviour uses the very same Space/mouse route.
  ImGui::GetIO().ConfigMacOSXBehaviors = true;
  ui.key(app, ImGuiKey_Space, true); ui.press(app, empty);
  ui.hover(app, {empty.x + 25, empty.y + 25}); ui.release(app); ui.key(app, ImGuiKey_Space, false);
  CHECK(app.previewPanX == 25 && app.previewPanY == 25);
  return true;
}

bool doubleClicks() {
  App app; Ui ui; CHECK(fixture(app)); ui.tick(app); ui.tick(app);
  const auto before = app.history.size();
  for (auto kind : {PreviewDrag::Centre, PreviewDrag::Left, PreviewDrag::Right,
                    PreviewDrag::Top, PreviewDrag::Bottom, PreviewDrag::Feather}) {
    app.previewZoom = 1.5f; app.previewPanX = 37; app.previewPanY = -19; ui.tick(app);
    const auto point = ui.grip(app, kind);
    ui.press(app, point); ui.release(app); ui.press(app, point);
    CHECK(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
    CHECK(app.previewInteraction.drag == kind);
    CHECK(app.previewZoom == 1.5f && app.previewPanX == 37 && app.previewPanY == -19);
    ui.release(app);
  }
  CHECK(app.history.size() == before);
  const ImVec2 background{ui.canvas.Min.x + 30, ui.canvas.Min.y + 30};
  ui.press(app, background); ui.release(app); ui.press(app, background);
  CHECK(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
  CHECK(app.previewZoom == 1 && app.previewPanX == 0 && app.previewPanY == 0);
  ui.release(app);
  return true;
}

bool interruptionsAndAlignment() {
  App app; Ui ui; CHECK(fixture(app)); ui.tick(app); ui.tick(app);
  CHECK(!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard));
  const auto id = *app.selectedNodeId();
  for (int interruption = 0; interruption < 7; ++interruption) {
    selectNode(app, id); ui.visible = ui.texture = true; ui.tick(app);
    const auto start = ui.grip(app, PreviewDrag::Centre); const auto before = app.history.size();
    ui.press(app, start); ui.hover(app, {start.x + 15, start.y - 10});
    const auto edited = parameter(app, id, "centreX");
    if (interruption == 0) selectNode(app, app.primarySource);
    if (interruption == 1) ui.visible = false;
    if (interruption == 2) ui.texture = false;
    if (interruption == 3) {
      const auto editedY = parameter(app, id, "centreY");
      ui.key(app, ImGuiKey_Escape, true);
      CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
      CHECK(app.previewInteraction.drag == PreviewDrag::None && app.history.size() == before + 1);
      CHECK(parameter(app, id, "centreX") == edited && parameter(app, id, "centreY") == editedY);
    }
    if (interruption == 4) finishUiEdits(app);
    if (interruption == 5) ImGui::GetIO().AddFocusEvent(false);
    if (interruption == 6) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ui.tick(app); ui.hover(app, {start.x + 40, start.y - 30}); ui.release(app);
    CHECK(parameter(app, id, "centreX") == edited && app.history.size() == before + 1);
    CHECK(!app.previewInteraction.editPending && app.previewInteraction.drag == PreviewDrag::None);
    if (interruption == 3) ui.key(app, ImGuiKey_Escape, false);
    if (interruption == 5) { ImGui::GetIO().AddFocusEvent(true); ui.tick(app); }
  }
  selectNode(app, id); ui.visible = ui.texture = true; ui.tick(app);
  auto start = ui.grip(app, PreviewDrag::Centre); ui.press(app, start); ui.hover(app, {start.x + 10, start.y});
  const auto accepted = parameter(app, id, "centreX");
  const auto before = app.history.size();
  {
    auto exporting = app.renderer.acquireExport(); CHECK(bool(exporting));
    ui.hover(app, {start.x + 40, start.y}); ui.release(app);
    CHECK(parameter(app, id, "centreX") == accepted && app.history.size() == before);
    CHECK(app.previewInteraction.editPending && !canUndoGrade(app));
  }
  ui.tick(app); CHECK(app.history.size() == before + 1 && !app.previewInteraction.editPending);
  app.sidecarWriteBlockedPath = app.path;
  ui.drag(app, PreviewDrag::Centre, {20, 20}); CHECK(parameter(app, id, "centreX") == accepted);
  const ImVec2 empty{ui.canvas.Min.x + 20, ui.canvas.Min.y + 20};
  ui.press(app, empty, 2); ui.hover(app, {empty.x + 20, empty.y + 10}); ui.release(app, 2);
  CHECK(app.previewPanX == 20 && app.previewPanY == 10);
  app.previewPanX = app.previewPanY = 0;
  app.sidecarWriteBlockedPath.clear();
  ui.tick(app);
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  // Successful grade replacement reusing the same node IDs cancels old input.
  PersistGrade grade; CHECK(captureGrade(app, grade));
  ui.tick(app); start = ui.grip(app, PreviewDrag::Centre); ui.press(app, start); ui.hover(app, {start.x + 10, start.y});
  CHECK(applyGrade(app, grade)); selectNode(app, id); ui.tick(app);
  const auto restored = parameter(app, id, "centreX");
  ui.hover(app, {start.x + 50, start.y}); ui.release(app);
  CHECK(parameter(app, id, "centreX") == restored && !app.previewInteraction.editPending);
  ui.tick(app); ui.press(app, ui.grip(app, PreviewDrag::Centre));
  // Direct history replay preserves a valid selection. Even a still-unedited
  // press cannot retain its baseline across replacement with reused IDs.
  CHECK(app.history.undo(app));
  const auto replayed = parameter(app, id, "centreX");
  ui.hover(app, {start.x + 70, start.y}); ui.release(app);
  CHECK(parameter(app, id, "centreX") == replayed && !app.previewInteraction.editPending);
  return true;
}

bool savedGeometryAndPixels() {
  Files files; App app; Ui ui; CHECK(fixture(app));
  const auto radial = *app.selectedNodeId();
  selectOutput(app); CHECK(addNativeExposureNode(app)); const auto exposure = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, exposure, "exposure", 1.0));
  CHECK(graph::connect(app, {radial, ports::mask}, {exposure, std::nullopt, ports::mask}).ok());
  selectNode(app, radial); ui.tick(app); ui.tick(app);
  // The centre moves exactly one eighth of the displayed width and height.
  const auto size = ui.view.size;
  ui.drag(app, PreviewDrag::Centre, {size.x / 8, -size.y / 8});
  CHECK(near(parameter(app, radial, "centreX"), .625) && near(parameter(app, radial, "centreY"), .625));
  const auto evaluate = [&](int w, int h) {
    Image source; source.w = w; source.h = h;
    for (int i = 0; i < w * h; ++i) source.px.insert(source.px.end(), {.25f, -.5f, 2, .375f});
    return evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
        {EvaluationPurpose::Preview, 96, 48, w, h}, std::make_shared<Image>(std::move(source)));
  };
  const auto image = [](const EvaluationResult &result) { return std::get<ImageValue>(result.value); };
  const auto smallResult = evaluate(32,16);
  CHECK(smallResult.maskPlacements.size() == 2);
  const auto small = image(smallResult), large = image(evaluate(96,48));
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 32; ++x) {
    // Same normalised pixel centre at (3*x+1,3*y+1), including negative/HDR and alpha.
    for (int c = 0; c < 4; ++c) CHECK(small->px[(y * 32 + x) * 4 + c] == large->px[((3 * y + 1) * 96 + 3 * x + 1) * 4 + c]);
  }
  // Independent oracle at a pixel centre: d=hypot(1/16,1/8), below inner d=.5.
  CHECK(small->px[(10 * 32 + 20) * 4] == .5f);
  CHECK(small->px[(10 * 32 + 20) * 4 + 1] == -1 && small->px[(10 * 32 + 20) * 4 + 2] == 4);
  CHECK(small->px[(10 * 32 + 20) * 4 + 3] == .375f && small->px[0] == .25f);
  PersistGrade saved; CHECK(captureGrade(app, saved));
  std::string original, after, error; CHECK(encodeTransferPayload(saved, original, error));
  const auto history = app.history.size();
  // Merely showing controls in every mode, and navigating, changes no grade.
  for (auto mode : {PreviewMode::Photo, PreviewMode::MaskOverlay, PreviewMode::MaskOnly}) {
    CHECK(setPreviewMode(app, mode)); ui.tick(app); ui.tick(app);
    CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  }
  CHECK(setPreviewMode(app, PreviewMode::Photo));
  CHECK(savePreset(app, files.path("geometry.rawnode.json")));
  PersistGrade loaded; CHECK(loadPresetFile(files.path("geometry.rawnode.json"), loaded, error));
  CHECK(encodeTransferPayload(loaded, after, error) && original == after && app.history.size() == history);
  App restored; restored.path = "restored.png"; restored.full = app.full; restored.preview = app.preview;
  CHECK(applyGrade(restored, loaded));
  for (const char *name : {"centreX", "centreY", "radiusX", "radiusY", "feather"})
    CHECK(parameter(app, radial, name) == parameter(restored, radial, name));
  Image result; CHECK(renderChain(restored, app.full, result).ok && result.px == small->px);
  app.outputEncoding = {RgbGamut::Rec709, TransferFunction::Linear}; app.exportFormat = 0;
  CHECK(runExportJob(app, files.path("export.png")));
  Image exported; ColorEncoding encoding; bool raw;
  CHECK(loadImage(files.path("export.png"), exported, encoding, raw));
  // PNG quantisation; the processor result itself remains HDR/unclipped above.
  CHECK(std::abs(exported.px[(10 * 32 + 20) * 4] - .5f) <= 1.0f / 255);
  CHECK(std::abs(exported.px[(10 * 32 + 20) * 4 + 3] - .375f) <= 1.0f / 255);
  CHECK(!std::filesystem::exists(files.path("export.png.rawnode.json")));
  // The preview worker receives edits through the same production invalidation.
  app.renderer.start(); app.renderer.requestPreview();
  CHECK(RenderRuntimeTestAccess::wait(app.renderer, [](auto s) { return !s.previewPending && !s.previewBusy && !s.displayPending; }));
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(image(app.processed.result)->px == small->px);
  }
  app.renderer.shutdown();
  // Actual same-photo reopening replaces its processors, clears the gesture and
  // establishes a fresh history even when the path and restored IDs are reused.
  App photo; CHECK(writeImage(app.full, files.path("photo.png")));
  CHECK(openPath(photo, files.path("photo.png")) && addNativeRadialMaskNode(photo));
  const auto oldId = *photo.selectedNodeId();
  selectOutput(photo); CHECK(addNativeExposureNode(photo));
  CHECK(graph::connect(photo,{oldId,ports::mask},{*photo.selectedNodeId(),std::nullopt,ports::mask}).ok());
  selectNode(photo,oldId); ui.tick(photo); ui.tick(photo);
  const auto start = ui.grip(photo, PreviewDrag::Centre);
  ui.press(photo, start); ui.hover(photo, {start.x + 10, start.y});
  CHECK(openPath(photo, photo.path)); selectNode(photo, oldId); ui.tick(photo);
  const auto reopened = parameter(photo, oldId, "centreX");
  ui.hover(photo, {start.x + 50, start.y}); ui.release(photo);
  CHECK(parameter(photo, oldId, "centreX") == reopened && photo.history.size() == 1);
  CHECK(!photo.previewInteraction.editPending && photo.previewInteraction.drag == PreviewDrag::None);
  return true;
}

bool affineGeometryAndDrag() {
  const ui::PreviewTransform view{{100,200},{800,400}};
  const auto mapping = CoordinateMapping::affine(0,-2,1.25,1.5,.5,-.25);
  const ui::RadialGeometry g{.5,.5,.25,.125,.5};
  const auto handles = ui::radialHandles(view,g,mapping);
  // Literal transformed cardinal points; this ellipse is rotated and sheared.
  const ImVec2 expected[] = {{300,300},{300,450},{300,150},{100,275},{500,325}};
  for (int i = 0; i < 5; ++i) {
    CHECK(handles[i].anchor.x == expected[i].x && handles[i].anchor.y == expected[i].y);
    CHECK(ui::hitRadialHandle(handles,handles[i].grip) == handles[i].kind);
  }
  for (double scale : {1.0,.5}) {
    const auto outline = ui::radialOutline(view,g,mapping,scale);
    for (const auto &p : outline) {
      // Independent analytic inverse: u=1.25-2*y; v=1.5*x+.5*y-.25.
      const double u = (p.x-100)/800, v = 1-(p.y-200)/400;
      const double y = (1.25-u)/2, x = (v+.25-.5*y)/1.5;
      const double ellipse = std::pow((x-.5)/.25,2) + std::pow((y-.5)/.125,2);
      CHECK(std::abs(ellipse-scale*scale) < 2e-6);
    }
  }
  const auto reflection = CoordinateMapping::affine(-1,0,1,0,1,0);
  const auto reflected = ui::radialHandles(view,g,reflection);
  CHECK(reflected[1].anchor.x == 700 && reflected[2].anchor.x == 300);
  CHECK(!(CoordinateMapping::affine(1,0,1e16,0,1,0) == CoordinateMapping::affine(2,0,1e16,0,1,0)));
  for (const auto &transform : {mapping,reflection}) {
    const auto collapsed = ui::radialHandles(view,{.5,.5,0,0,1},transform);
    for (const auto &h : collapsed) CHECK(ui::hitRadialHandle(collapsed,h.grip) == h.kind);
  }
  App app; Ui ui; CHECK(fixture(app)); const auto mask = *app.selectedNodeId();
  const auto shear = CoordinateMapping::affine(.5,-.5,.5,.5,.75,-.125);
  ui.mappingOverride = shear; ui.tick(app); ui.tick(app);
  CHECK(ui::radialPlacement(app).mapping == shear);
  auto size = ui.view.size; auto count = app.history.size();
  // du=.09375,dv=.015625 is independently the image displacement of
  // mask displacement (+.125,-.0625) under this affine.
  ui.drag(app,PreviewDrag::Centre,{size.x*.09375f,-size.y*.015625f});
  CHECK(near(parameter(app,mask,"centreX"),.625) && near(parameter(app,mask,"centreY"),.4375));
  CHECK(app.history.size() == count+1);
  CHECK(app.history.undo(app) && parameter(app,mask,"centreX") == .5 && parameter(app,mask,"centreY") == .5);
  CHECK(app.history.redo(app)); ui.tick(app);
  ui.hover(app,ui.grip(app,PreviewDrag::Right));
  CHECK(ImGui::GetMouseCursor() == ImGuiMouseCursor_ResizeNESW);
  ui.drag(app,PreviewDrag::Right,{size.x*.03125f,-size.y*.03125f});
  CHECK(near(parameter(app,mask,"radiusX"),.3125));
  CHECK(setNodeParameterValue(app,mask,"radiusX",.25)); ui.tick(app);
  // Equal radii put the mapped feather spoke exactly vertical. Horizontal
  // motion must not change feather, despite nonzero inverse-mask X/Y motion.
  ui.drag(app,PreviewDrag::Feather,{30,0}); CHECK(near(parameter(app,mask,"feather"),.5));
  ui.drag(app,PreviewDrag::Feather,{0,float(size.y*1.25*.25*.25/std::sqrt(2.0))});
  CHECK(std::abs(parameter(app,mask,"feather")-.75) < .02);
  CHECK(setNodeParameterValue(app,mask,"radiusX",0.0) && setNodeParameterValue(app,mask,"radiusY",0.0));
  ui.tick(app); ui.drag(app,PreviewDrag::Right,{size.x*.03125f,-size.y*.03125f});
  CHECK(near(parameter(app,mask,"radiusX"),.0625));
  ui.drag(app,PreviewDrag::Top,{-size.x*.03125f,-size.y*.046875f});
  CHECK(near(parameter(app,mask,"radiusY"),.0625));
  ui.mappingOverride = reflection; app.renderer.requestPreview(); ui.tick(app);
  const auto previous = parameter(app,mask,"centreX");
  ui.drag(app,PreviewDrag::Centre,{size.x/16,0});
  CHECK(near(parameter(app,mask,"centreX"),previous-.0625));
  ui.drag(app,PreviewDrag::Left,{size.x/16,0});
  CHECK(near(parameter(app,mask,"radiusX"),.125));
  return true;
}

bool sharedPlacementAndPublication() {
  App app; Ui ui; CHECK(fixture(app)); ui.tick(app); ui.autoPublish = false;
  const auto mask = *app.selectedNodeId();
  const auto placements = [&](std::vector<CoordinateMapping> mappings) {
    publishFrame(app);
    {
      std::lock_guard<std::mutex> lock(app.displayMutex);
      auto &records = app.processed.result.maskPlacements; records.clear();
      for (size_t i = 0; i < mappings.size(); ++i)
        records.push_back({{mask,ports::mask},{NodeId{"consumer-"+std::to_string(i)},std::nullopt,ports::mask},mappings[i]});
    }
    fakeUpload(app);
  };
  const auto identity = CoordinateMapping::identity(), moved = CoordinateMapping::affine(1,0,.125,0,1,0);
  const auto unknown = CoordinateMapping::unknown();
  placements({identity,identity}); CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  placements({moved,moved}); CHECK(ui::radialPlacement(app).mapping == moved);
  for (auto values : {std::vector<CoordinateMapping>{identity,moved}, {identity,unknown}, {unknown,identity},
                      {CoordinateMapping::affine(0,0,0,0,1,0)}}) {
    placements(values); const auto p = ui::radialPlacement(app);
    CHECK(p.state == ui::PlacementState::Unavailable && p.message);
  }
  placements({identity,unknown});
  const auto hiddenCentre = ui.view.toScreen(.5,.5);
  ui.press(app,hiddenCentre); ui.hover(app,{hiddenCentre.x+30,hiddenCentre.y}); ui.release(app);
  CHECK(parameter(app,mask,"centreX") == .5);
  placements({}); CHECK(ui::radialPlacement(app).state == ui::PlacementState::Unavailable);
  placements({identity});
  const int before = app.textureGeneration;
  app.renderer.requestPreview(); CHECK(ui::radialPlacement(app).state == ui::PlacementState::Pending);
  // New CPU metadata cannot become interactive over old GPU pixels.
  auto request = std::make_shared<OutputRef>(app.documentOutput);
  auto result = app.processed.result;
  display_detail::publishPreview(app,result,RenderRuntimeTestAccess::snapshot(app.renderer).epoch,
      app.renderer.previewDemand(),request,{{},32,16});
  CHECK(app.textureGeneration == before && ui::radialPlacement(app).state == ui::PlacementState::Pending);
  fakeUpload(app); CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  app.renderer.requestDisplayRefresh(); CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  CHECK(setPreviewMode(app,PreviewMode::MaskOnly));
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Pending);
  publishFrame(app); CHECK(ui::radialPlacement(app).mapping == identity);
  CHECK(setPreviewMode(app,PreviewMode::Photo)); publishFrame(app);
  // Escape must finish accepted edits even while a newly published CPU result
  // has not reached the texture. A pending frame pauses edits, not Escape.
  ui.autoPublish = true; ui.tick(app); ui.tick(app);
  auto escapeStart = ui.grip(app,PreviewDrag::Centre);
  const auto beforeEscape = app.history.size();
  ui.press(app,escapeStart); ui.hover(app,{escapeStart.x+20,escapeStart.y});
  const auto accepted = parameter(app,mask,"centreX");
  ui.autoPublish = false;
  display_detail::publishPreview(app,app.processed.result,RenderRuntimeTestAccess::snapshot(app.renderer).epoch,
      app.renderer.previewDemand(),std::make_shared<OutputRef>(app.documentOutput),{{},32,16});
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Pending);
  ui.autoUpload = false; // Model a CPU publication arriving after this frame's upload phase.
  ui.key(app,ImGuiKey_Escape,true);
  ui.autoUpload = true;
  CHECK(app.previewInteraction.drag == PreviewDrag::None && app.history.size() == beforeEscape+1);
  CHECK(parameter(app,mask,"centreX") == accepted);
  ui.release(app); ui.key(app,ImGuiKey_Escape,false);
  // Completing a deferred Inspector value on this press changes demand. The
  // same press must not also begin a drag against the older displayed frame.
  ui.autoPublish = true; ui.tick(app); ui.tick(app);
  const auto inspectorStart = ui.grip(app,PreviewDrag::Centre);
  const auto beforeInspector = app.history.size();
  app.parameterGesture = ui::ParameterGesture{0x1234,app.path,mask,"centreX",ParameterValue{.6}};
  ui.press(app,inspectorStart); ui.hover(app,{inspectorStart.x+20,inspectorStart.y}); ui.release(app);
  CHECK(parameter(app,mask,"centreX") == .6 && app.history.size() == beforeInspector+1);
  CHECK(!app.parameterGesture);
  // Pending own edits retain one gesture; a resolution/image generation from
  // elsewhere interrupts it, even when the eventual mapping would be equal.
  ui.autoPublish = true; ui.tick(app); ui.tick(app);
  auto start = ui.grip(app,PreviewDrag::Centre); ui.press(app,start);
  ui.hover(app,{start.x+20,start.y}); const double edited = parameter(app,mask,"centreX");
  ui.autoPublish = false;
  app.preview = std::make_shared<Image>(*app.preview); app.renderer.requestPreview();
  ui.hover(app,{start.x+60,start.y}); ui.release(app);
  CHECK(parameter(app,mask,"centreX") == edited && !app.previewInteraction.editPending);
  CHECK(app.previewInteraction.drag == PreviewDrag::None);
  return true;
}

bool runtimeGenerationPairing() {
  App app; Ui ui; CHECK(fixture(app)); ui.autoPublish = false;
  struct Gate {
    App &app;
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked = false, entered = false;
    void release() { { std::lock_guard<std::mutex> lock(mutex); blocked = false; } cv.notify_all(); }
    ~Gate() { release(); app.renderer.shutdown(); }
  } gate{app};
  RenderRuntimeTestAccess::publication(app.renderer,[](void *context,const OutputRef &) {
    auto &gate = *static_cast<Gate *>(context);
    std::unique_lock<std::mutex> lock(gate.mutex);
    if (!gate.blocked) return;
    gate.entered = true; gate.cv.notify_all();
    gate.cv.wait(lock,[&] { return !gate.blocked; });
  },&gate);
  const auto idle = [&] { return RenderRuntimeTestAccess::wait(app.renderer,[](auto s) {
    return !s.previewPending && !s.previewBusy && !s.displayPending;
  }); };
  app.renderer.start(); CHECK(idle()); fakeUpload(app);
  ui.tick(app); ui.tick(app);
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  const int uploaded = app.textureGeneration;
  { std::lock_guard<std::mutex> lock(gate.mutex); gate.blocked = true; }
  app.renderer.requestPreview();
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    CHECK(gate.cv.wait_for(lock,std::chrono::seconds(2),[&] { return gate.entered; }));
  }
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Pending);
  const auto mask = *app.selectedNodeId();
  selectNode(app,app.documentOutput.nodeId); ui.tick(app);
  selectNode(app,mask); ui.tick(app);
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Pending);
  const auto centre = ui.view.toScreen(.5,.5);
  ui.press(app,centre); ui.hover(app,{centre.x+30,centre.y-20}); ui.release(app);
  CHECK(parameter(app,mask,"centreX") == .5 && parameter(app,mask,"centreY") == .5);
  // Supersede this blocked render before publication, through real demand.
  app.renderer.requestPreview(); gate.release(); CHECK(idle());
  CHECK(app.textureGeneration == uploaded && app.processed.generation == RenderRuntimeTestAccess::snapshot(app.renderer).epoch);
  CHECK(app.processed.demand == app.renderer.previewDemand());
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Pending);
  fakeUpload(app); CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  return true;
}


bool appendCrop(App &app, const char *id, double amount, double x, double y);

bool liveWorkerHandleDrag(bool delayed, bool maskOnly = false, bool escape = false,
                          bool overlay = false, bool transformed = false) {
  App app; Ui ui; CHECK(fixture(app)); ui.autoPublish = false;
  const auto mask = *app.selectedNodeId();
  if (overlay) CHECK(setNodeParameterValue(app,mask,"invert",true));
  if (transformed) CHECK(appendCrop(app,"live-crop",50,0,0));
  const double inverseScale = transformed ? .5 : 1;
  const auto expectedMapping = transformed ? CoordinateMapping::affine(2,0,-.5,0,2,-.5)
                                           : CoordinateMapping::identity();
  if (maskOnly) CHECK(setPreviewMode(app,PreviewMode::MaskOnly));
  if (overlay) CHECK(setPreviewMode(app,PreviewMode::MaskOverlay));
  struct Gate {
    App &app;
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked = false;
    int entered = 0;
    void block() { std::lock_guard<std::mutex> lock(mutex); blocked = true; }
    void release() { { std::lock_guard<std::mutex> lock(mutex); blocked = false; } cv.notify_all(); }
    int count() { std::lock_guard<std::mutex> lock(mutex); return entered; }
    bool wait(int count) {
      std::unique_lock<std::mutex> lock(mutex);
      return cv.wait_for(lock,std::chrono::seconds(2),[&] { return entered >= count; });
    }
    ~Gate() {
      release(); app.renderer.shutdown();
      RenderRuntimeTestAccess::observe(app.renderer,nullptr,nullptr);
    }
  } gate{app};
  RenderRuntimeTestAccess::publication(app.renderer,[](void *context,const OutputRef &) {
    auto &gate = *static_cast<Gate *>(context);
    std::unique_lock<std::mutex> lock(gate.mutex);
    if (!gate.blocked) return;
    ++gate.entered; gate.cv.notify_all();
    gate.cv.wait(lock,[&] { return !gate.blocked; });
  },&gate);
  // A regressed gesture can try to capture history while this test holds the
  // worker. Give that drain an independent release path rather than deadlock.
  RenderRuntimeTestAccess::observe(app.renderer,&gate,[](void *context,auto event) noexcept {
    if (event == RenderRuntimeTestAccess::Event::MutationWaiting)
      static_cast<Gate *>(context)->release();
  });
  const auto idle = [&] { return RenderRuntimeTestAccess::wait(app.renderer,[](auto s) {
    return !s.previewPending && !s.previewBusy && !s.displayPending;
  }); };
  app.renderer.start(); CHECK(idle()); fakeUpload(app);
  if (overlay) {
    CHECK(app.processed.overlayMask && app.processed.overlayMask->state == EvaluationState::Available);
    if (transformed) {
      CHECK(std::get<MaskValue>(app.processed.overlayMask->value)->w == 32 && app.texW == 16);
      const auto background = display_detail::preparePresentation(app,app.processed.result);
      CHECK(app.displayRGBA != background.rgba);
    }
  }
  gate.block(); ui.tick(app); ui.tick(app);
  const auto start = ui.grip(app,PreviewDrag::Centre), size = ui.view.size;
  const auto history = app.history.size();
  ui.press(app,start); CHECK(app.previewInteraction.drag == PreviewDrag::Centre);
  int expectedPublication = gate.count()+1;
  for (int i = 1; i <= 6; ++i) {
    ui.hover(app,{start.x+10*i,start.y-5*i});
    if (!delayed || i == 1) CHECK(gate.wait(expectedPublication));
    if (!delayed) {
      // Normal order: finish the real render, upload its presentation, then
      // draw another UI frame with the button held and no further movement.
      gate.release(); CHECK(idle()); gate.block();
      expectedPublication = gate.count()+1;
    }
    ui.tick(app); // Dispatch has advanced the epoch; ownership must survive it.
    CHECK(app.previewInteraction.drag == PreviewDrag::Centre && app.history.size() == history);
    CHECK(near(parameter(app,mask,"centreX"),.5+inverseScale*10*i/size.x));
    CHECK(near(parameter(app,mask,"centreY"),.5+inverseScale*5*i/size.y));
    CHECK(app.previewInteraction.editDemand == app.renderer.previewDemand());
  }
  const auto completedDemand = app.renderer.previewDemand();
  const auto completedTexture = app.textureGeneration;
  if (escape) {
    // Escape retains accepted moves with either completed or pending work.
    ui.key(app,ImGuiKey_Escape,true);
    gate.release(); CHECK(idle()); fakeUpload(app); gate.block();
    ui.release(app); ui.key(app,ImGuiKey_Escape,false);
  } else ui.release(app);
  gate.release(); CHECK(idle()); fakeUpload(app);
  const double finalX = .5+inverseScale*60/size.x, finalY = .5+inverseScale*30/size.y;
  CHECK(app.previewInteraction.drag == PreviewDrag::None && !app.previewInteraction.editPending);
  CHECK(app.history.size() == history+1);
  if (!delayed) {
    CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch > completedTexture);
    CHECK(app.renderer.previewDemand() == completedDemand && app.textureGeneration == completedTexture);
  }
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  CHECK(app.processed.demand == app.renderer.previewDemand());
  gate.block(); CHECK(app.history.undo(app)); gate.release(); CHECK(idle()); fakeUpload(app);
  CHECK(parameter(app,mask,"centreX") == .5 && parameter(app,mask,"centreY") == .5);
  gate.block(); CHECK(app.history.redo(app)); gate.release(); CHECK(idle()); fakeUpload(app);
  CHECK(near(parameter(app,mask,"centreX"),finalX) && near(parameter(app,mask,"centreY"),finalY));
  CHECK(app.history.size() == history+1);

  // An independent request must interrupt even though the resulting Image
  // mapping is unchanged. Later held-button input cannot resume it.
  gate.block(); ui.tick(app);
  const auto again = ui.grip(app,PreviewDrag::Centre);
  const auto interruptedHistory = app.history.size();
  expectedPublication = gate.count()+1;
  ui.press(app,again); ui.hover(app,{again.x+10,again.y-5});
  CHECK(gate.wait(expectedPublication));
  CHECK(app.previewInteraction.drag == PreviewDrag::Centre);
  const double acceptedX = parameter(app,mask,"centreX"), acceptedY = parameter(app,mask,"centreY");
  app.renderer.requestPreview();
  ui.hover(app,{again.x+50,again.y-25});
  CHECK(app.previewInteraction.drag == PreviewDrag::None && app.history.size() == interruptedHistory+1);
  CHECK(parameter(app,mask,"centreX") == acceptedX && parameter(app,mask,"centreY") == acceptedY);
  gate.release(); CHECK(idle()); fakeUpload(app); gate.block();
  CHECK(ui::radialPlacement(app).mapping == expectedMapping);
  ui.hover(app,{again.x+80,again.y-40}); ui.release(app);
  CHECK(parameter(app,mask,"centreX") == acceptedX && parameter(app,mask,"centreY") == acceptedY);
  CHECK(app.history.size() == interruptedHistory+1);
  std::printf("ok  Live-worker Radial drag: %s, %s, %s, six moves, one Undo/Redo, external interruption\n",
              delayed ? "pending publication" : "render and upload between frames",
              overlay ? (transformed ? "cropped Mask Overlay" : "Mask Overlay") : (maskOnly ? "Mask Only" : "Photo"),
              escape ? "Escape" : "release");
  return true;
}

bool liveWorkerIdleCaptures() {
  App app; Ui ui; CHECK(fixture(app)); ui.autoPublish = false;
  struct Guard { App &app; ~Guard() { app.renderer.shutdown(); } } guard{app};
  const auto idle = [&] { return RenderRuntimeTestAccess::wait(app.renderer,[](auto s) {
    return !s.previewPending && !s.previewBusy && !s.displayPending;
  }); };
  const auto mask = *app.selectedNodeId(), effect = app.documentOutput.nodeId;
  app.renderer.start(); CHECK(idle()); ui.tick(app); ui.tick(app);
  // Model an Inspector slider edit whose Preview is already uploaded when its
  // gesture completes and captures history. No new request is needed on capture.
  selectNode(app,effect);
  app.parameterGesture = ui::ParameterGesture{0x1234,app.path,effect,"exposure",std::nullopt};
  CHECK(setNodeParameterValue(app,effect,"exposure",1.0));
  CHECK(idle()); fakeUpload(app);
  const auto generation = app.textureGeneration;
  const auto demand = app.renderer.previewDemand();
  const auto history = app.history.size();
  CHECK(finishParameterGesture(app,true));
  CHECK(app.history.size() == history+1 && !app.parameterGesture);
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch > generation);
  CHECK(app.renderer.previewDemand() == demand && app.processed.demand == demand);
  selectNode(app,mask); ui.tick(app);
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);

  // These are the read-only capture operations used by Copy Node / Copy Grade.
  PersistFragment fragment; std::vector<std::string> warnings; PersistGrade grade;
  const auto epoch = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
  CHECK(captureNodeFragment(app,{mask},fragment,warnings) && captureGrade(app,grade));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch > epoch);
  CHECK(app.renderer.previewDemand() == demand && app.processed.demand == demand);
  CHECK(app.processed.generation == generation && app.textureGeneration == generation);
  selectNode(app,effect); ui.tick(app); selectNode(app,mask); ui.tick(app);
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);

  const auto request = app.processed.request;
  app.renderer.requestDisplayRefresh(); CHECK(idle()); ui.tick(app);
  CHECK(app.processed.demand == demand && app.renderer.previewDemand() == demand);
  CHECK(app.processed.generation == generation && app.processed.request == request);
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  // Independent drags must still start after idle capture, with their existing
  // history and Escape semantics. The six-move cases exercise live ownership.
  const auto start = ui.grip(app,PreviewDrag::Centre), size = ui.view.size;
  ui.press(app,start); ui.hover(app,{start.x+20,start.y-10}); ui.release(app);
  CHECK(idle()); ui.tick(app);
  CHECK(near(parameter(app,mask,"centreX"),.5+20/size.x));
  CHECK(near(parameter(app,mask,"centreY"),.5+10/size.y));
  CHECK(app.history.size() == history+2 && ui::radialPlacement(app).state == ui::PlacementState::Ready);
  CHECK(app.history.undo(app) && idle()); ui.tick(app);
  CHECK(parameter(app,mask,"centreX") == .5 && parameter(app,mask,"centreY") == .5);
  CHECK(app.history.redo(app) && idle()); ui.tick(app);
  CHECK(near(parameter(app,mask,"centreX"),.5+20/size.x));
  CHECK(near(parameter(app,mask,"centreY"),.5+10/size.y));
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  std::printf("ok  Live-worker idle Inspector/Copy captures, node switching, display demand and subsequent drag\n");
  return true;
}

bool previewDemandRequests() {
  App app;
  const auto initial = app.renderer.previewDemand();
  app.renderer.requestPreview(); CHECK(app.renderer.previewDemand() == initial+1);
  app.renderer.requestDisplayRefresh(); CHECK(app.renderer.previewDemand() == initial+1);
  app.renderer.requestInspection({app.primarySource,ports::image});
  CHECK(app.renderer.previewDemand() == initial+2);
  app.renderer.requestDocumentPreview(); CHECK(app.renderer.previewDemand() == initial+3);
  app.renderer.shutdown();
  app.renderer.requestPreview(); app.renderer.requestInspection({app.primarySource,ports::image});
  app.renderer.requestDocumentPreview();
  CHECK(app.renderer.previewDemand() == initial+3);
  return true;
}

bool appendCrop(App &app,const char *id,double amount = 0,double x = 0,double y = 0) {
  int index = -1;
  for (size_t i = 0; i < gPlugins.size(); ++i) if (gPlugins[i].label == "Crop") index = int(i);
  Node crop; crop.id = NodeId{id}; crop.processor = OfxProcessor::create(index); CHECK(crop.processor);
  CHECK(crop.processor->setParameterValue("crop",amount) && crop.processor->setParameterValue("offsetX",x) &&
        crop.processor->setParameterValue("offsetY",y));
  return graph::insertImageNode(app,std::move(crop)).ok();
}

bool realCropPlacements(bool invert) {
  App app; Ui ui; CHECK(fixture(app)); const auto mask = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app,mask,"invert",invert));
  CHECK(appendCrop(app,"crop-a",50,50,-100)); selectNode(app,mask); ui.tick(app); ui.tick(app);
  auto placement = ui::radialPlacement(app); CHECK(placement.state == ui::PlacementState::Ready);
  // [12,0,28,8] in 32x16 -> 16x8. The normalized map is identical
  // to PR #107's independent 8x6 window [3,0,7,3] -> 4x3.
  CHECK(placement.mapping == CoordinateMapping::affine(2,0,-.75,0,2,0));
  auto point = placement.mapping.map(.5,.25); CHECK(point && point->first == .25 && point->second == .5);
  CHECK(setPreviewMode(app,PreviewMode::MaskOverlay)); ui.tick(app);
  CHECK(ui::radialPlacement(app).mapping == placement.mapping && app.processed.overlayMask);
  // The literal Crop window selects original sample centres (12+x+.5,y+.5).
  // Evaluate the documented default radial equation there, independently of
  // both placement inversion and the overlay sampler. Handles above use the
  // forward mapping from the very same publication.
  const auto background = display_detail::preparePresentation(app,app.processed.result);
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 16; ++x) {
    const double d = std::hypot(((12+x+.5)/32-.5)/.25,((y+.5)/16-.5)/.25);
    const double t = std::clamp(2*(1-d),0.0,1.0);
    const double ordinary = t*t*(3-2*t);
    const float amount = .4f*float(invert ? 1-ordinary : ordinary);
    const size_t i = (size_t(7-y)*16+x)*4;
    for (int c = 0; c < 3; ++c) {
      const float target = c == 0 ? 255.0f : 0.0f;
      CHECK(app.displayRGBA[i+c] == static_cast<unsigned char>(background.rgba[i+c]+(target-background.rgba[i+c])*amount+.5f));
    }
    CHECK(app.displayRGBA[i+3] == background.rgba[i+3]);
  }
  CHECK(setPreviewMode(app,PreviewMode::Photo)); ui.tick(app);
  const double originalX = parameter(app,mask,"centreX"), originalY = parameter(app,mask,"centreY");
  auto size = ui.view.size;
  ui.drag(app,PreviewDrag::Centre,{size.x/8,-size.y/8});
  CHECK(near(parameter(app,mask,"centreX"),originalX+.0625) && near(parameter(app,mask,"centreY"),originalY+.0625));
  CHECK(appendCrop(app,"crop-b",50,-100,0)); selectNode(app,mask); ui.tick(app);
  placement = ui::radialPlacement(app);
  // Second local [0,2,8,6] -> 8x4. Independent composition 4u-1.5,4v-.5.
  CHECK(placement.state == ui::PlacementState::Ready && placement.mapping == CoordinateMapping::affine(4,0,-1.5,0,4,-.5));
  CHECK(setPreviewMode(app,PreviewMode::MaskOnly)); ui.tick(app);
  CHECK(ui::radialPlacement(app).mapping == CoordinateMapping::identity() && app.texW == 32 && app.texH == 16);
  CHECK(setPreviewMode(app,PreviewMode::MaskOverlay)); ui.tick(app);
  CHECK(ui::radialPlacement(app).state == ui::PlacementState::Ready);
  CHECK(ui::radialPlacement(app).mapping == CoordinateMapping::affine(4,0,-1.5,0,4,-.5));
  CHECK(setPreviewMode(app,PreviewMode::Photo));
  // Crop before Mask application with an explicit Reference changes generation
  // dimensions, not saved normalized geometry or placement at the consumer.
  CHECK(graph::deleteImageNode(app,NodeId{"crop-b"}).ok());
  const auto effect = findNode(app,NodeId{"crop-a"})->fixedInputs.at(ports::image).value();
  CHECK(graph::reconnect(app,{app.primarySource,ports::image},{NodeId{"crop-a"},std::nullopt,ports::image}).ok());
  CHECK(graph::reconnect(app,{NodeId{"crop-a"},ports::image},{effect.nodeId,std::nullopt,ports::image}).ok());
  CHECK(graph::connect(app,{NodeId{"crop-a"},ports::image},{mask,std::nullopt,ports::reference}).ok());
  CHECK(graph::setDocumentOutput(app,{effect.nodeId,ports::image}).ok());
  const auto x = parameter(app,mask,"centreX"), y = parameter(app,mask,"centreY"); ui.tick(app);
  CHECK(ui::radialPlacement(app).mapping == CoordinateMapping::identity() && app.texW == 16 && app.texH == 8);
  CHECK(parameter(app,mask,"centreX") == x && parameter(app,mask,"centreY") == y);
  const auto result = evaluateGraph({app.nodes,app.primarySource,app.documentOutput},{mask,ports::mask},
      {EvaluationPurpose::Preview,32,16,32,16},app.preview);
  CHECK(result.state == EvaluationState::Available);
  const auto raster = std::get<MaskValue>(result.value); CHECK(raster->w == 16 && raster->h == 8);
  return true;
}

bool sameSizeBackends() {
  App app; Ui ui; CHECK(fixture(app)); const auto mask = *app.selectedNodeId();
  app.full.w = 40; app.full.h = 53; app.full.px.assign(40*53*4,.25f);
  app.preview = std::make_shared<Image>(app.full);
  CHECK(appendCrop(app,"identity-crop"));
  CHECK(setNodeStrength(app,NodeId{"identity-crop"},.5)); selectNode(app,mask); ui.tick(app);
  CHECK(ui::radialPlacement(app).mapping == CoordinateMapping::identity());
  Files files; const auto path = files.path("identity.ctl");
  { std::ofstream script(path); script <<
      "void main(input varying float rIn,input varying float gIn,input varying float bIn,"
      "output varying float rOut,output varying float gOut,output varying float bOut,"
      "output varying float aOut,input varying float aIn=1.0)"
      "{rOut=rIn;gOut=gIn;bOut=bIn;aOut=aIn;}\n"; }
  selectOutput(app); CHECK(addCtlNode(app,path)); selectNode(app,mask); ui.tick(app);
  CHECK(ui::radialPlacement(app).mapping == CoordinateMapping::identity());
  // Resolution changes preserve normalized placement and use the new texture's
  // actual dimensions. Pan/zoom do not change graph placement or saved geometry.
  app.preview = std::make_shared<Image>();
  Image small; small.w = 16; small.h = 8; small.px.assign(16*8*4,.25f);
  app.preview = std::make_shared<Image>(std::move(small)); app.renderer.requestPreview(); ui.tick(app);
  CHECK(app.texW == 16 && app.texH == 8 && ui::radialPlacement(app).mapping == CoordinateMapping::identity());
  app.previewZoom = 2; app.previewPanX = 37; app.previewPanY = -19; ui.tick(app);
  const double before = parameter(app,mask,"centreX");
  const float size = ui.view.size.x;
  ui.drag(app,PreviewDrag::Centre,{size/8,0});
  CHECK(near(parameter(app,mask,"centreX"),before+.125));
  return true;
}

}

bool testPreviewInteraction() {
  if (!liveWorkerHandleDrag(false) || !liveWorkerHandleDrag(false,false,true) ||
      !liveWorkerHandleDrag(false,true) || !liveWorkerHandleDrag(true) ||
      !liveWorkerHandleDrag(true,false,true) || !liveWorkerHandleDrag(false,false,false,true) ||
      !liveWorkerHandleDrag(true,false,true,true) || !liveWorkerIdleCaptures() || !previewDemandRequests() ||
      !mappingsAndOverlap() || !collapsedRadiusRecovery() || !editsAndHistory() || !doubleClicks() ||
      !ownershipAndZoom() || !interruptionsAndAlignment() ||
      !savedGeometryAndPixels() || !affineGeometryAndDrag() || !sharedPlacementAndPublication() || !runtimeGenerationPairing()) return false;
  std::printf("ok  Preview mappings, Radial handles, mouse ownership, history and interruptions\n");
  return true;
}


bool testPreviewCoordinatePlacement() {
  if (!realCropPlacements(false) || !realCropPlacements(true) || !sameSizeBackends() ||
      !liveWorkerHandleDrag(false,false,false,true,true) || !liveWorkerHandleDrag(true,false,true,true,true)) return false;
  std::printf("ok  Preview real Crop/CTL placement, inverse edits, Reference and resolution\n");
  return true;
}

int runPreviewInteractionTests() {
  loadPlugins();
  const bool ok = testPreviewInteraction() && testPreviewCoordinatePlacement();
  bool unloaded = true;
  for (auto &entry : gPlugins) {
    const auto status = callAction(entry.plugin,kOfxActionUnload,nullptr);
    unloaded &= status == kOfxStatOK || status == kOfxStatReplyDefault;
  }
  gPlugins.clear();
  return ok && unloaded ? 0 : 1;
}
