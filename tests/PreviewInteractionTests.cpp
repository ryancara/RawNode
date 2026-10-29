#include "PreviewInteractionTests.h"

#include "AppState.h"
#include "Export.h"
#include "GraphCommands.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActions.h"
#include "ui/UiContext.h"
#include "imgui_internal.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

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
void fixture(App &app) {
  app.path = "preview-handles.png";
  app.full.w = 32; app.full.h = 16;
  for (int i = 0; i < 32 * 16; ++i) app.full.px.insert(app.full.px.end(), {.25f, -.5f, 2, .375f});
  app.preview = std::make_shared<Image>(app.full);
  addNativeRadialMaskNode(app);
  app.history.reset(app);
}
ui::RadialGeometry geometry(const App &app) {
  const auto id = *app.selectedNodeId();
  return {parameter(app, id, "centreX"), parameter(app, id, "centreY"), parameter(app, id, "radiusX"),
          parameter(app, id, "radiusY"), parameter(app, id, "feather")};
}

// Real Preview and ImGui input, without a GL context. Only texture upload is
// omitted: the fake texture supplies the dimensions of the displayed raster.
struct Ui {
  ImGuiContext *previous = ImGui::GetCurrentContext(), *context = ImGui::CreateContext();
  ImRect canvas;
  ui::PreviewTransform view;
  bool visible = true, otherControls = false, texture = true;
  ImVec2 textPoint;
  char text[32] = {};
  Ui() {
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char *pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  }
  ~Ui() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
  void tick(App &app) {
    auto &io = ImGui::GetIO(); io.DisplaySize = {1400, 900}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    if (visible) {
      app.displayDirty = false; app.tex = texture ? 1 : 0; app.texW = 32; app.texH = 16;
      ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({912, 650});
      ImGui::Begin("Preview test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      drawPreviewPanel(app);
      canvas = context->LastItemData.Rect;
      const float fit = std::min(canvas.GetWidth() / 32, canvas.GetHeight() / 16);
      const ImVec2 size{32 * fit * app.previewZoom, 16 * fit * app.previewZoom};
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
    for (const auto &h : ui::radialHandles(view, geometry(app))) if (h.kind == kind) return h.grip;
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
    for (double radius : {0.0, .001, .25}) for (double feather : {0.0, .5, 1.0}) {
      const auto handles = ui::radialHandles(scaled, {.5, .5, radius, radius, feather});
      for (const auto &h : handles) CHECK(ui::hitRadialHandle(handles, h.grip) == h.kind);
      CHECK(handles[0].anchor.x == 100 + size.x / 2 && handles[0].anchor.y == 200 + size.y / 2);
      CHECK(std::abs(handles[5].anchor.x - (100 + size.x * (.5 + radius * (1 - feather) / std::sqrt(2.0)))) < 1e-4);
    }
  }
  return true;
}

bool editsAndHistory() {
  App app; Ui ui; fixture(app); ui.tick(app); ui.tick(app);
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
  App app; Ui ui; fixture(app); ui.tick(app); ui.tick(app);
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
  CHECK(app.previewZoom > 2); // Double clicks on handles never reset zoom.
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

bool interruptionsAndAlignment() {
  App app; Ui ui; fixture(app); ui.tick(app); ui.tick(app);
  const auto id = *app.selectedNodeId();
  for (int interruption = 0; interruption < 6; ++interruption) {
    selectNode(app, id); ui.visible = ui.texture = true; ui.tick(app);
    const auto start = ui.grip(app, PreviewDrag::Centre); const auto before = app.history.size();
    ui.press(app, start); ui.hover(app, {start.x + 15, start.y - 10});
    const auto edited = parameter(app, id, "centreX");
    if (interruption == 0) selectNode(app, app.primarySource);
    if (interruption == 1) ui.visible = false;
    if (interruption == 2) ui.texture = false;
    if (interruption == 3) ui.key(app, ImGuiKey_Escape, true);
    if (interruption == 4) finishUiEdits(app);
    if (interruption == 5) ImGui::GetIO().AddFocusEvent(false);
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
  CHECK(ui::radialHandlesAligned(app));
  selectOutput(app);
  CHECK(addNativeExposureNode(app)); const auto exposure = *app.selectedNodeId();
  selectNode(app, id); CHECK(ui::radialHandlesAligned(app));
  selectOutput(app);
  CHECK(addNativeCstNode(app)); selectNode(app, id); CHECK(ui::radialHandlesAligned(app));
  // Unknown same-sized Image processing cannot imply Source coordinates.
  Node unknown(NodeOperation::ImageProcessor); unknown.id = NodeId{"unknown"};
  unknown.fixedInputs[ports::image] = OutputRef{app.primarySource, ports::image};
  app.nodes.push_back(std::move(unknown)); app.documentOutput = {NodeId{"unknown"}, ports::image};
  CHECK(!ui::radialHandlesAligned(app));
  findNode(app, NodeId{"unknown"})->enabled = false; CHECK(ui::radialHandlesAligned(app));
  findNode(app, NodeId{"unknown"})->enabled = true;
  findNode(app, NodeId{"unknown"})->strength = 0; CHECK(ui::radialHandlesAligned(app));
  findNode(app, NodeId{"unknown"})->strength = .5; CHECK(!ui::radialHandlesAligned(app));
  CHECK(setPreviewMode(app, PreviewMode::MaskOnly) && ui::radialHandlesAligned(app));
  CHECK(setPreviewMode(app, PreviewMode::MaskOverlay) && !ui::radialHandlesAligned(app));
  CHECK(setPreviewMode(app, PreviewMode::Photo));
  app.documentOutput = {exposure, ports::image};
  findNode(app, id)->fixedInputs[ports::reference] = OutputRef{NodeId{"unknown"}, ports::image};
  CHECK(!ui::radialHandlesAligned(app));
  app.documentOutput = {NodeId{"unknown"}, ports::image}; CHECK(ui::radialHandlesAligned(app));
  findNode(app, id)->fixedInputs[ports::reference].reset(); app.documentOutput = {exposure, ports::image};
  findNode(app, id)->enabled = false; CHECK(ui::radialHandlesAligned(app));
  findNode(app, id)->enabled = true;
  // Successful grade replacement reusing the same node IDs cancels old input.
  app.nodes.erase(std::remove_if(app.nodes.begin(), app.nodes.end(),
      [](const auto &n) { return n.id.value == "unknown"; }), app.nodes.end());
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
  Files files; App app; Ui ui; fixture(app);
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
  const auto small = image(evaluate(32, 16)), large = image(evaluate(96, 48));
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
    CHECK(ui::radialHandlesAligned(app));
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
  const auto oldId = *photo.selectedNodeId(); ui.tick(photo); ui.tick(photo);
  const auto start = ui.grip(photo, PreviewDrag::Centre);
  ui.press(photo, start); ui.hover(photo, {start.x + 10, start.y});
  CHECK(openPath(photo, photo.path)); selectNode(photo, oldId); ui.tick(photo);
  const auto reopened = parameter(photo, oldId, "centreX");
  ui.hover(photo, {start.x + 50, start.y}); ui.release(photo);
  CHECK(parameter(photo, oldId, "centreX") == reopened && photo.history.size() == 1);
  CHECK(!photo.previewInteraction.editPending && photo.previewInteraction.drag == PreviewDrag::None);
  return true;
}
}

bool testPreviewInteraction() {
  if (!mappingsAndOverlap() || !editsAndHistory() || !ownershipAndZoom() || !interruptionsAndAlignment() ||
      !savedGeometryAndPixels()) return false;
  std::printf("ok  Preview mappings, Radial handles, mouse ownership, history and interruptions\n");
  return true;
}
