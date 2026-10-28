#include "HistoryUiTests.h"

#include "AppState.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActionsPriv.h"
#include "ui/UiContext.h"
#include "ui/ParamWidgets.h"

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
  bool visible = true, full = false, opacityOnly = false;
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
      if (opacityOnly) {
        ImGui::LogToBuffer(0); drawNodeOpacity(app, *selectedNode(app));
        log = context->LogBuffer.c_str(); ImGui::LogFinish();
      }
      else drawInspector(app);
      item = context->LastItemData.Rect;
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

bool opacityGesturesAndPersistence() {
  Files files; App app; Ui ui; ui.opacityOnly = true;
  const auto photo = files.photo("opacity.png");
  CHECK(openPath(app, photo) && addNativeExposureNode(app));
  const auto id = *app.selectedNodeId();
  ui.tick(app); ui.tick(app); const auto row = ui.item;
  const auto before = app.history.size();
  ui.hover(app, row.GetCenter()); ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app);
  for (int i = 0; i < 5; ++i) {
    ui.hover(app, {row.GetCenter().x + i * 5, row.GetCenter().y});
    CHECK(app.history.size() == before && !canUndoGrade(app));
  }
  ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app);
  double strength = findNode(app, id)->strength;
  CHECK(strength > 0 && strength < 1 && app.history.size() == before + 1 && !app.parameterGesture);
  CHECK(app.history.undo(app) && findNode(app, id)->strength == 1);
  CHECK(app.history.redo(app) && findNode(app, id)->strength == strength);
  // Use the visible pencil, as for ordinary numeric processor parameters.
  ui.tick(app);
  const float h = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
  const ImVec2 pencil{ui.item.Min.x - gap - h / 2, ui.item.GetCenter().y};
  const auto numericBefore = app.history.size();
  ui.click(app, pencil); ui.tick(app); ui.type(app, "37.5");
  CHECK(findNode(app, id)->strength == strength && app.history.size() == numericBefore);
  ui.key(app, ImGuiKey_Enter);
  CHECK(findNode(app, id)->strength == .375 && app.history.size() == numericBefore + 1 && !app.parameterGesture);
  CHECK(ui.log.find("37.5%") != std::string::npos && exposure(app, id) == 0);
  CHECK(app.history.undo(app) && findNode(app, id)->strength == strength);
  CHECK(app.history.redo(app) && findNode(app, id)->strength == .375);
  ui.click(app, pencil); ui.tick(app); ui.type(app, "150"); ui.key(app, ImGuiKey_Enter);
  CHECK(findNode(app, id)->strength == 1);
  ui.click(app, pencil); ui.tick(app); ui.type(app, "-10"); ui.key(app, ImGuiKey_Enter);
  CHECK(findNode(app, id)->strength == 0);
  ui.click(app, pencil); ui.tick(app); ui.type(app, "34.375"); ui.key(app, ImGuiKey_Enter);
  strength = findNode(app, id)->strength;
  CHECK(strength == .34375 && ui.log.find("34.375%") != std::string::npos);
  const auto unchanged = app.history.size();
  ui.click(app, pencil); ui.tick(app); ui.type(app, "80"); ui.key(app, ImGuiKey_Escape);
  CHECK(findNode(app, id)->strength == strength && app.history.size() == unchanged);
  ui.click(app, pencil); ui.tick(app); ui.type(app, "62.5");
  ui.visible = false; ui.tick(app); // Disappearing numeric controls complete the same gesture.
  strength = findNode(app, id)->strength;
  CHECK(strength == .625 && app.history.size() == unchanged + 1 && !app.parameterGesture);
  CHECK(!setNodeStrength(app, id, -1) && !setNodeStrength(app, id, 2));
  CHECK(saveCurrentInputSidecar(app));
  const auto preset = (files.dir / "opacity-preset.rawnode.json").string();
  CHECK(savePreset(app, preset));
  PersistGrade grade; std::string error;
  CHECK(loadPresetFile(preset, grade, error));
  App restored; CHECK(applyGrade(restored, grade));
  CHECK(findNode(restored, id)->strength == strength);
  CHECK(openPath(app, photo) && findNode(app, id)->strength == strength);
  return true;
}

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

bool radialInspectorGestures() {
  Files files; App app; Ui ui;
  CHECK(openPath(app, files.photo("radial.png")) && addNativeRadialMaskNode(app));
  const auto id = *app.selectedNodeId();
  const auto feather = [&] { return std::get<double>(findNode(app, id)->processor->parameters()[4].value); };
  ui.tick(app); ui.tick(app); // The last Inspector row is Feather.
  const auto row = ui.item;
  const auto before = app.history.size();
  ui.hover(app, row.GetCenter()); ImGui::GetIO().AddMouseButtonEvent(0, true); ui.tick(app);
  ui.hover(app, {row.GetCenter().x + 35, row.GetCenter().y});
  CHECK(app.history.size() == before);
  ImGui::GetIO().AddMouseButtonEvent(0, false); ui.tick(app);
  const double dragged = feather();
  CHECK(dragged != .5 && app.history.size() == before + 1 && !app.parameterGesture);
  CHECK(app.history.undo(app) && feather() == .5);
  CHECK(app.history.redo(app) && feather() == dragged);
  ui.tick(app);
  const float h = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
  const ImVec2 pencil{ui.item.Min.x - 2.5f * h - 3 * gap, ui.item.GetCenter().y};
  ui.click(app, pencil); ui.tick(app); ui.type(app, "0.375");
  CHECK(feather() == dragged);
  ui.key(app, ImGuiKey_Enter);
  CHECK(feather() == .375 && !app.parameterGesture);
  CHECK(app.history.undo(app) && feather() == dragged);
  CHECK(app.history.redo(app) && feather() == .375);
  ui.full = true; ui.tick(app); ui.tick(app);
  CHECK(ui.log.find("Radial Mask") != std::string::npos && ui.log.find("Centre X") != std::string::npos);
  CHECK(ui.log.find("Horizontal radius") != std::string::npos && ui.log.find("Feather") != std::string::npos);
  CHECK(ui.log.find("Opacity") == std::string::npos);
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

bool droppedPhotoCompletesOutgoingGesture() {
  for (const auto decision : {SaveFailureDecision::Discard, SaveFailureDecision::Cancel}) {
    Files files; App app; Ui ui;
    const auto path = files.photo("drop.png"), script = (files.dir / "exposure.ctl").string();
    std::ofstream(script) << "void main(input varying float rIn, input varying float gIn, input varying float bIn, "
        "output varying float rOut, output varying float gOut, output varying float bOut, "
        "input uniform float exposure = 1.25) { rOut=rIn*exposure; gOut=gIn*exposure; bOut=bIn*exposure; }";
    CHECK(openPath(app, path) && addCtlNode(app, script));
    const auto id = *app.selectedNodeId();
    CHECK(saveCurrentInputSidecar(app));
    std::ifstream savedFile(inputSidecarPath(path), std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(savedFile), {}};
    savedFile.close();
    PersistGrade native; CHECK(captureGrade(app, native));
    for (auto &node : native.graph.nodes) if (node.id == id.value) {
      node.backend = "native"; node.identifier = "rawnode.native.exposure"; node.label = "Exposure";
      node.paramsJson = {{"exposure", "0"}};
    }
    CHECK(applyGrade(app, native) && selectNode(app, id));
    CHECK(findNode(app, id)->processor->backend() == ProcessorBackend::Native && exposure(app, id) == 0);
    ui.tick(app); ui.tick(app);
    const float h = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    ui.click(app, {28 + 1.5f * h + gap, ui.item.GetCenter().y}); ui.tick(app); ui.type(app, "7");
    CHECK(app.parameterGesture && app.parameterGesture->pendingValue == std::optional<ParameterValue>{7.0});
    CHECK(exposure(app, id) == 0);
    const auto size = app.history.size(), cursor = app.history.cursor();
    int saves = 0, prompts = 0, mutations = 0;
    bool completedBeforeSave = false, completedBeforeDecision = false;
    RenderRuntimeTestAccess::observe(app.renderer, &mutations, [](void *context, auto event) noexcept {
      if (event == RenderRuntimeTestAccess::Event::MutationCompleted) ++*static_cast<int *>(context);
    });
    document_detail::ActionTestOptions options;
    options.beforeSave = [&] {
      ++saves;
      completedBeforeSave = !app.parameterGesture && exposure(app, id) == 7 &&
          app.history.size() == size + 1 && app.history.cursor() == cursor + 1 && !app.history.hasUnrecordedEdit();
      return persist_detail::FileFault::Replace;
    };
    // This is the dispatch called by GLFW, between ImGui frames. A failed save
    // must see the outgoing value before the user chooses Discard or Cancel.
    const bool opened = openDroppedPhoto(app, path, [&](const std::string &error) {
      ++prompts;
      completedBeforeDecision = !error.empty() && completedBeforeSave && !app.parameterGesture && exposure(app, id) == 7;
      return decision;
    }, options);
    ui.tick(app); ui.tick(app);
    if (decision == SaveFailureDecision::Discard) {
      CHECK(opened && findNode(app, id)->processor->backend() == ProcessorBackend::CTL);
      CHECK(findNode(app, id)->processor->identifier() == std::filesystem::canonical(script).string() && exposure(app, id) == 1.25);
      CHECK(app.history.size() == 1 && app.history.cursor() == 0 && !app.history.hasUnrecordedEdit());
      CHECK(!app.history.canUndo() && !app.history.canRedo() && mutations == 3);
    } else {
      CHECK(!opened && findNode(app, id)->processor->backend() == ProcessorBackend::Native && exposure(app, id) == 7);
      // Cancel may leave the unchanged input focused. Ending that focus adds no step.
      ui.key(app, ImGuiKey_Enter);
      CHECK(app.history.size() == size + 1 && app.history.cursor() == cursor + 1 && mutations == 2);
      CHECK(app.history.canUndo() && !app.history.hasUnrecordedEdit());
    }
    CHECK(saves == 1 && prompts == 1 && completedBeforeSave && completedBeforeDecision && !app.parameterGesture);
    std::ifstream afterFile(inputSidecarPath(path), std::ios::binary);
    CHECK((std::string{std::istreambuf_iterator<char>(afterFile), {}}) == saved);
    RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  }
  std::puts("ok  History UI: GLFW drop dispatch completes numeric input before saving; Discard reopens CTL without NodeId reuse leakage; Cancel retains outgoing state");
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
  if (!radialInspectorGestures() || !opacityGesturesAndPersistence() || !nativeGestures() || !ctlDeferredInputAndFailure() || !droppedPhotoCompletesOutgoingGesture() || !shortcutsAndMenus()) return false;
  std::puts("ok  History UI: native gestures, numeric commits/interruption, discrete buttons, shortcuts and eligibility");
  return true;
}
