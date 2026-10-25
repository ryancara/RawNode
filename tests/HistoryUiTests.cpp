#include "HistoryUiTests.h"

#include "AppState.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/UiContext.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "history UI check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("rawnode-history-ui-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { std::filesystem::create_directory(dir); }
  ~Files() { std::error_code ignored; std::filesystem::remove_all(dir, ignored); }
  std::string photo(const char *name) {
    Image image; image.w = image.h = 1; image.px = {.25f, -.5f, 2, .375f};
    const auto path = (dir / name).string();
    if (!writeImage(image, path, {RgbGamut::Rec709, TransferFunction::Linear})) throw std::runtime_error("fixture image");
    return path;
  }
};
double exposure(const App &app, const NodeId &id) {
  return std::get<double>(findNode(app, id)->processor->parameters()[0].value);
}
struct Ui {
  ImGuiContext *previous = ImGui::GetCurrentContext(), *context = ImGui::CreateContext();
  ImRect item;
  bool visible = true, full = false;
  std::string log;
  char text[64] = {};
  ImVec2 textPoint;
  Ui() {
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char *pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }
  ~Ui() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
  void tick(App &app) {
    auto &io = ImGui::GetIO(); io.DisplaySize = {1400, 900}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    if (full) {
      ImGui::LogToBuffer(0); DrawUiFrame(app); log = context->LogBuffer.c_str(); ImGui::LogFinish();
    }
    else if (visible) {
      ImGui::SetNextWindowPos({20, 40}); ImGui::SetNextWindowSize({650, 720});
      ImGui::Begin("History Inspector", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
      drawInspector(app); item = context->LastItemData.Rect;
      ImGui::Button("Focus outside parameter");
      ImGui::End();
    }
    if (full) {
      ImGui::SetNextWindowPos({1000, 650}); ImGui::SetNextWindowSize({350, 150});
      ImGui::Begin("History text input");
      ImGui::InputText("Text", text, sizeof text); textPoint = ImGui::GetItemRectMin(); textPoint.x += 20; textPoint.y += 8;
      ImGui::End();
    }
    finishParameterGesture(app);
    ui::finishGraphDrag(app);
    ImGui::EndFrame();
  }
  void hover(App &app, ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); tick(app); }
  void click(App &app, ImVec2 p) {
    hover(app, p); ImGui::GetIO().AddMouseButtonEvent(0, true); tick(app);
    ImGui::GetIO().AddMouseButtonEvent(0, false); tick(app);
  }
  void key(App &app, ImGuiKey key, bool shift = false, bool modifier = false) {
    auto &io = ImGui::GetIO();
    if (modifier) io.AddKeyEvent(io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, true);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(key, true); tick(app); io.AddKeyEvent(key, false);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
    if (modifier) io.AddKeyEvent(io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, false);
    tick(app);
  }
  void type(App &app, const char *value) {
    key(app, ImGuiKey_A, false, true);
    ImGui::GetIO().AddInputCharactersUTF8(value); tick(app);
  }
};

bool nativeGestures() {
  Files files; App app; Ui ui;
  CHECK(openPath(app, files.photo("native.png")) && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  ui.tick(app); ui.tick(app);
  const auto slider = ui.item;
  const ImVec2 start{slider.GetCenter().x, slider.GetCenter().y};
  const auto size = app.history.size();
  int mutations = 0;
  RenderRuntimeTestAccess::observe(app.renderer, &mutations, [](void *context, auto event) noexcept {
    if (event == RenderRuntimeTestAccess::Event::MutationCompleted) ++*static_cast<int *>(context);
  });
  ui.hover(app, start); ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app);
  for (int i = 1; i <= 24; ++i) {
    ui.hover(app, {start.x + i * 2.0f, start.y});
    CHECK(app.history.size() == size && mutations == 0 && !canUndoGrade(app));
  }
  const auto completed = exposure(app, id); CHECK(completed > 0);
  ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app);
  CHECK(app.history.size() == size + 1 && mutations == 1 && !app.parameterGesture);
  CHECK(app.history.undo(app) && exposure(app, id) == 0 && app.history.canRedo());
  CHECK(app.history.redo(app) && exposure(app, id) == completed);
  RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);

  // Numeric popup writes once on Enter, focus loss or disappearance; Escape
  // uses ImGui's restored value. Pencil/reset/step positions follow the actual row.
  ui.tick(app); const auto row = ui.item;
  const float h = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
  const ImVec2 pencil{28 + 1.5f * h + gap, row.GetCenter().y};
  ui.click(app, pencil); ui.tick(app); ui.type(app, "2");
  CHECK(exposure(app, id) == completed);
  const auto typedSize = app.history.size();
  ui.key(app, ImGuiKey_Enter);
  CHECK(exposure(app, id) == 2 && app.history.size() == typedSize + 1);
  ui.click(app, pencil); ui.tick(app); ui.type(app, "3"); ui.key(app, ImGuiKey_Escape);
  CHECK(exposure(app, id) == 2 && app.history.size() == typedSize + 1);
  ui.click(app, pencil); ui.tick(app); ui.type(app, "4"); ui.visible = false; ui.tick(app);
  CHECK(exposure(app, id) == 4 && !app.history.hasUnrecordedEdit());
  ui.visible = true; ui.tick(app);
  ui.click(app, pencil); ui.tick(app); ui.type(app, "4.5"); ui.click(app, {500, 650});
  CHECK(exposure(app, id) == 4.5 && !app.history.hasUnrecordedEdit());
  ui.click(app, pencil); ui.tick(app); ui.type(app, "5"); selectNode(app, app.primarySource); ui.tick(app);
  CHECK(exposure(app, id) == 5 && !app.parameterGesture && !app.history.hasUnrecordedEdit());
  selectNode(app, id); ui.tick(app);
  const auto beforeStep = app.history.size();
  ui.click(app, {28 + 2.5f * h + 2 * gap, ui.item.GetCenter().y});
  CHECK(app.history.size() == beforeStep + 1 && exposure(app, id) < 5 && exposure(app, id) > 0);
  ui.click(app, {28 + h * .5f, ui.item.GetCenter().y});
  CHECK(exposure(app, id) == 0 && app.history.size() == beforeStep + 2);
  const auto unchanged = app.history.size();
  ui.click(app, {28 + h * .5f, ui.item.GetCenter().y});
  CHECK(app.history.size() == unchanged);
  return true;
}

bool ctlDeferredInputAndFailure() {
  Files files; App app; Ui ui;
  const auto path = files.photo("ctl.png"), script = (files.dir / "gain.ctl").string();
  std::ofstream(script) << "void main(input varying float rIn, input varying float gIn, input varying float bIn, "
      "output varying float rOut, output varying float gOut, output varying float bOut, "
      "input uniform float gain = 3.0) { rOut=rIn*gain; gOut=gIn*gain; bOut=bIn*gain; }";
  CHECK(openPath(app, path) && addCtlNode(app, script)); const auto id = *app.selectedNodeId();
  const auto value = [&] { return std::get<double>(findNode(app, id)->processor->parameters()[0].value); };
  CHECK(!findNode(app, id)->processor->parameters()[0].hasRange);
  ui.tick(app); ui.tick(app);
  const auto size = app.history.size();
  ui.click(app, {ui.item.Min.x + 20, ui.item.GetCenter().y}); ui.type(app, "20");
  CHECK(value() == 3 && app.history.size() == size);
  ui.key(app, ImGuiKey_Enter);
  CHECK(value() == 20 && app.history.size() == size + 1);
  CHECK(app.history.undo(app) && value() == 3);
  CHECK(app.history.redo(app) && value() == 20);
  ui.tick(app); ui.click(app, {ui.item.Min.x + 20, ui.item.GetCenter().y}); ui.type(app, "21");
  {
    auto exporting = app.renderer.acquireExport(); CHECK(bool(exporting));
    app.setStatus("Export in progress");
    ui.visible = false; ui.tick(app);
    CHECK(value() == 20 && app.parameterGesture && app.getStatus() == "Export in progress");
  }
  ui.tick(app); CHECK(value() == 21 && !app.parameterGesture && !app.history.hasUnrecordedEdit());
  ui.visible = true; ui.tick(app);
  // A removed reconstruction asset preserves both actual value and cursor.
  const auto cursor = app.history.cursor(); std::filesystem::remove(script);
  CHECK(!app.history.undo(app) && app.history.cursor() == cursor && value() == 21);
  CHECK(app.getStatus().find("Could not reconstruct") != std::string::npos);

  Files other; CHECK(openPath(app, other.photo("next.png")) && app.history.size() == 1);
  CHECK(!app.parameterGesture && !canUndoGrade(app));
  CHECK(addNativeExposureNode(app)); const auto native = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, native, "exposure", 1.0)); CHECK(app.history.recordCompletedEdit(app));
  // Cap failure retains the completed live gesture and pauses traversal once.
  app.history = GradeHistory{100, 2000}; CHECK(app.history.reset(app));
  findNode(app, native)->preservedParamsJson["large"] = jsonStringValue(std::string(4096, 'x'));
  selectNode(app, native); ui.tick(app);
  const auto start = ui.item.GetCenter(); ui.hover(app, start);
  ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app); ui.hover(app, {start.x + 30, start.y});
  ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app);
  CHECK(app.history.hasUnrecordedEdit() && !app.parameterGesture && !canUndoGrade(app));
  const auto status = app.getStatus(); ui.tick(app); CHECK(app.getStatus() == status);
  return true;
}

bool shortcutsAndMenus() {
  for (const bool mac : {false, true}) {
    Files files; App app; Ui ui; ui.full = true;
    app.showRight = app.showNodes = app.showFilmstrip = false;
    ImGui::GetIO().ConfigMacOSXBehaviors = mac;
    CHECK(openPath(app, files.photo("keys.png")) && addNativeExposureNode(app));
    ui.tick(app); ui.tick(app);
    CHECK(canUndoGrade(app) && !canRedoGrade(app));
    ui.key(app, ImGuiKey_Z, false, true);
    CHECK(app.nodes.size() == 1 && !canUndoGrade(app) && canRedoGrade(app));
    ui.key(app, ImGuiKey_Z, true, true);
    CHECK(app.nodes.size() == 2 && canUndoGrade(app));
    ui.key(app, ImGuiKey_Z, false, true); ui.key(app, ImGuiKey_Y, false, true);
    CHECK(app.nodes.size() == (mac ? 1u : 2u));
    if (mac) ui.key(app, ImGuiKey_Z, true, true);
    ui.click(app, ui.textPoint); ImGui::GetIO().AddInputCharactersUTF8("abc"); ui.tick(app);
    ui.key(app, ImGuiKey_Z, false, true);
    CHECK(app.nodes.size() == 2); // ImGui text undo owns the chord.
    ui.click(app, {900, 600}); ui.tick(app);
    app.sidecarWriteBlockedPath = app.path;
    CHECK(!canUndoGrade(app) && !canRedoGrade(app));
    ui.key(app, ImGuiKey_Z, false, true); CHECK(app.nodes.size() == 2);
    app.sidecarWriteBlockedPath.clear();
    {
      auto exporting = app.renderer.acquireExport(); CHECK(bool(exporting));
      CHECK(!canUndoGrade(app) && !canRedoGrade(app));
      ui.key(app, ImGuiKey_Z, false, true); CHECK(app.nodes.size() == 2);
    }
    app.history.noteUnrecordedEdit();
    CHECK(!canUndoGrade(app)); ui.key(app, ImGuiKey_Z, false, true); CHECK(app.nodes.size() == 2);
    CHECK(app.history.recordCompletedEdit(app));
    // Actual Edit menu disabled flags, including the platform shortcut text.
    ui.click(app, {65, 10});
    auto *menu = ImGui::FindWindowByName("##Menu_00"); CHECK(menu);
    CHECK(ui.log.find("Undo") != std::string::npos && ui.log.find(mac ? "⌘+Z" : "Ctrl+Z") != std::string::npos);
    const ImVec2 undo{menu->DC.CursorStartPos.x + 20, menu->DC.CursorStartPos.y + ImGui::GetTextLineHeight() / 2};
    const ImVec2 redo{undo.x, undo.y + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y};
    ui.click(app, redo); CHECK(app.nodes.size() == 2); // Disabled Redo.
    ui.click(app, undo); CHECK(app.nodes.size() == 1);
    ui.click(app, {65, 10}); ui.click(app, redo); CHECK(app.nodes.size() == 2);
    ui.key(app, ImGuiKey_Escape);
    app.history.clear();
    ui.key(app, ImGuiKey_Z, false, true); CHECK(app.nodes.size() == 2 && !canUndoGrade(app));
  }
  return true;
}
}

bool testHistoryUi() {
  if (!nativeGestures() || !ctlDeferredInputAndFailure() || !shortcutsAndMenus()) return false;
  std::puts("ok  History UI: native gestures, numeric commits/interruption, discrete buttons, shortcuts and eligibility");
  return true;
}
