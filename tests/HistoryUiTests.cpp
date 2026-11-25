#include "HistoryUiTests.h"

#include "AppState.h"
#include "GraphCommands.h"
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
  std::string log, clipboard;
  char text[64] = {};
  ImVec2 textPoint;
  Ui() {
    auto &io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigInputTrickleEventQueue = false;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    auto &platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &clipboard;
    platform.Platform_GetClipboardTextFn = [](ImGuiContext *context) {
      return static_cast<std::string *>(context->PlatformIO.Platform_ClipboardUserData)->c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext *context, const char *text) {
      *static_cast<std::string *>(context->PlatformIO.Platform_ClipboardUserData) = text;
    };
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
  void chordEvent(ImGuiKeyChord chord, bool down) {
    auto &io = ImGui::GetIO();
    // Feed physical modifier events, as the GLFW backend does.
    if (chord & ImGuiMod_Ctrl) io.AddKeyEvent(io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl, down);
    if (chord & ImGuiMod_Super) io.AddKeyEvent(io.ConfigMacOSXBehaviors ? ImGuiMod_Ctrl : ImGuiMod_Super, down);
    if (chord & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, down);
    io.AddKeyEvent(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_), down);
  }
  void chord(App &app, ImGuiKeyChord chord) {
    chordEvent(chord, true);
    tick(app);
    chordEvent(chord, false);
    tick(app);
  }
  void key(App &app, ImGuiKey key, bool shift = false, bool primary = false) {
    chord(app, key | (shift ? ImGuiMod_Shift : 0) | (primary ? ImGuiMod_Ctrl : 0));
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
  // Display range +/-5 EV, hard range +/-10 EV. Plus at +7 must move
  // forward by 1% of the ten-stop span (allow one double rounding).
  ui.click(app, pencil); ui.tick(app); ui.type(app, "7"); ui.key(app, ImGuiKey_Enter);
  ui.click(app, {28 + 3.5f * h + 3 * gap, ui.item.GetCenter().y});
  CHECK(std::abs(exposure(app, id) - 7.1) < 1e-14);
  return true;
}

bool radialInspectorBoolean() {
  Files files; App app; Ui ui;
  CHECK(openPath(app, files.photo("radial-invert.png")) && addNativeRadialMaskNode(app));
  const auto id = *app.selectedNodeId();
  const auto inverted = [&] {
    for (const auto &p : findNode(app, id)->processor->parameters())
      if (p.id == "invert") return std::get<bool>(p.value);
    throw std::runtime_error("Missing Invert parameter");
  };
  std::snprintf(app.paramFilter, sizeof app.paramFilter, "Invert");
  ui.tick(app); ui.tick(app);
  const auto history = app.history.size();
  CHECK(!inverted());
  ui.click(app, ui.item.GetCenter());
  CHECK(inverted() && app.history.size() == history + 1);
  CHECK(app.history.undo(app) && !inverted());
  CHECK(app.history.redo(app) && inverted());
  ui.tick(app);
  ui.click(app, {ui.item.Min.x - ImGui::GetStyle().ItemInnerSpacing.x - ImGui::GetFrameHeight()/2,
                 ui.item.GetCenter().y});
  CHECK(!inverted() && app.history.size() == history + 2);
  CHECK(app.history.undo(app) && inverted());
  CHECK(app.history.redo(app) && !inverted());
  return true;
}

bool radialInspectorGestures() {
  Files files; App app; Ui ui;
  CHECK(openPath(app, files.photo("radial.png")) && addNativeRadialMaskNode(app));
  const auto id = *app.selectedNodeId();
  const auto feather = [&] { return std::get<double>(findNode(app, id)->processor->parameters()[4].value); };
  std::snprintf(app.paramFilter, sizeof app.paramFilter, "Feather");
  ui.tick(app); ui.tick(app); // Select Feather independently of later processor parameters.
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
  app.paramFilter[0] = '\0';
  ui.full = true; ui.tick(app); ui.tick(app);
  CHECK(ui.log.find("Radial Mask") != std::string::npos && ui.log.find("Centre X") != std::string::npos);
  CHECK(ui.log.find("Horizontal radius") != std::string::npos && ui.log.find("Feather") != std::string::npos);
  CHECK(ui.log.find("Invert") != std::string::npos);
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
  using Command = ui::ApplicationCommand;
  ui::CommandStep target(Command::TargetParameter), amount(Command::AdjustValue), processor(Command::SelectProcessor);
  processor.operand = ui::ProcessorIdentity{"ctl", findNode(app, id)->processor->identifier()};
  target.operand = std::string("gain");
  amount.operand = -0.5;
  selectOutput(app);
  CHECK(ui::executeCommands(app, {processor, target, amount}) && value() == 20.5);
  CHECK(!ui::executeCommands(app, {Command::Increase, Command::ResetParameter}) && value() == 20.5);
  CHECK(ui::executeCommands(app, {Command::ResetParameter}) && value() == 3);
  amount.operand = 18.0;
  CHECK(ui::executeCommands(app, {amount}) && value() == 21);
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

bool orderedNodeCommands() {
  using Command = ui::ApplicationCommand;
  Files files; App app; Ui ui;
  CHECK(openPath(app, files.photo("nodes.png")) && addNativeExposureNode(app));
  const auto first = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app));
  const auto second = *app.selectedNodeId();
  // Put the second processor off-spine and reverse storage. The visible
  // List still starts at the first processor and includes the second.
  CHECK(graph::setDocumentOutput(app, {first, ports::image}).ok());
  std::reverse(app.nodes.begin(), app.nodes.end());
  ui::CommandStep processor(Command::SelectProcessor), target(Command::TargetParameter);
  ui::CommandStep amount(Command::AdjustValue), position(Command::SelectPosition);
  processor.operand = ui::ProcessorIdentity{"native", "rawnode.native.exposure"};
  target.operand = std::string("exposure");
  amount.operand = 0.5;
  position.operand = 2;
  const auto history = app.history.size();
  CHECK(ui::executeCommands(app, {processor, target, amount, Command::Increase}));
  CHECK(app.selectedNodeId() == second && app.history.size() == history + 2 && exposure(app, second) == 0.6);
  CHECK(ui::executeCommands(app, {Command::Undo}) && exposure(app, second) == 0.5 && app.targetedParameter.empty());
  CHECK(ui::executeCommands(app, {target, Command::Redo}) && exposure(app, second) == 0.6 && app.targetedParameter.empty());
  selectOutput(app);
  CHECK(ui::executeCommands(app, {processor, target, amount}) && app.selectedNodeId() == first);
  CHECK(!ui::executeCommands(app, {amount, Command::SelectPrevious, Command::ResetParameter}));
  CHECK(app.shortcutFailure == "Step 2 (Select Previous Node): No previous processing node");
  CHECK(exposure(app, first) == 1.0 && app.targetedParameter == "exposure");
  CHECK(ui::executeCommands(app, {Command::ResetParameter, position, target, Command::ResetParameter}));
  CHECK(exposure(app, first) == 0 && exposure(app, second) == 0);
  selectOutput(app);
  CHECK(app.targetedParameter.empty() && ui::executeCommands(app, {Command::SelectPrevious}) && app.selectedNodeId() == second);
  CHECK(!ui::executeCommands(app, {Command::SelectNext}));
  CHECK(selectNode(app, app.primarySource) && ui::executeCommands(app, {Command::SelectNext}) && app.selectedNodeId() == first);
  processor.operand = ui::ProcessorIdentity{"native", "missing"};
  CHECK(!ui::executeCommands(app, {processor}) && app.selectedNodeId() == first);
  target.operand = std::string("missing");
  CHECK(!ui::executeCommands(app, {target}));
  CHECK(app.shortcutFailure == "Step 1 (Target Parameter): Editable scalar parameter unavailable: missing");
  CHECK(!ui::executeCommands(app, {target, amount}) && app.selectedNodeId() == first);
  CHECK(app.shortcutFailure == "Step 1 (Target Parameter): Editable scalar parameter unavailable: missing");
  target.operand = std::string("exposure");
  amount.operand = 50.0;
  CHECK(ui::executeCommands(app, {target, amount}) && exposure(app, first) == 10);
  const auto atLimit = app.history.size();
  CHECK(ui::executeCommands(app, {Command::Increase}) && app.history.size() == atLimit);
  PersistGrade grade;
  CHECK(captureGrade(app, grade) && applyGrade(app, grade) && app.targetedParameter.empty());
  CHECK(selectNode(app, second) && ui::executeCommands(app, {target, Command::ToggleNode}));
  CHECK(!selectedNode(app)->enabled && destroyNode(app, second) && app.targetedParameter.empty());
  CHECK(selectNode(app, first) && ui::executeCommands(app, {target}));
  CHECK(openPath(app, files.photo("replacement.png")) && app.targetedParameter.empty());
  return true;
}

bool orderedPhotoCommands() {
  using Command = ui::ApplicationCommand;
  Files files; App app; Ui ui;
  const auto first = files.photo("first.png"), middle = files.photo("middle.png"), last = files.photo("last.png");
  app.filmstrip = {{first, PhotoMark::Unmarked}, {middle, PhotoMark::Reject}, {last, PhotoMark::Unmarked}};
  app.filmstripMarkFilter = PhotoMark::Unmarked;
  CHECK(ui::executeCommands(app, {Command::NextPhoto}) && app.path == first);
  CHECK(addNativeExposureNode(app));
  app.filmstripSelection = {first, middle, last};
  app.filmstripAnchor = last;
  CHECK(ui::executeCommands(app, {Command::MarkPick, Command::NextPhoto}));
  CHECK(app.path == last && app.filmstripSelection.size() == 1 && app.filmstripSelection.count(last));
  CHECK(app.filmstrip[0].mark == PhotoMark::Pick && !ui::executeCommands(app, {Command::NextPhoto}));
  app.filmstripMarkFilter.reset();
  CHECK(ui::executeCommands(app, {Command::PreviousPhoto, Command::PreviousPhoto}));
  CHECK(app.path == first && app.currentPhotoMark == PhotoMark::Pick && !ui::executeCommands(app, {Command::PreviousPhoto}));
  // A desktop clipboard can refuse a write. Never advance to paste stale data.
  auto &platform = ImGui::GetPlatformIO();
  const auto writeClipboard = platform.Platform_SetClipboardTextFn;
  platform.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *) {};
  CHECK(!ui::executeCommands(app, {Command::CopyFullGrade, Command::NextPhoto}) && app.path == first);
  platform.Platform_SetClipboardTextFn = writeClipboard;
  app.keyboardBindings = ui::KeyboardBindings{{ImGuiKey_F8, {Command::CopyFullGrade, Command::NextPhoto, Command::Paste}}};
  ui.full = true;
  ui.tick(app); ui.tick(app); ui.key(app, ImGuiKey_F8);
  CHECK(app.path == middle && app.nodes.size() == 2 && app.history.size() == 2);
  CHECK(ui::executeCommands(app, {Command::Undo}) && app.nodes.size() == 1);
  CHECK(ui::executeCommands(app, {Command::MarkReject, Command::NextPhoto}) && app.path == last);
  CHECK(app.filmstrip[1].mark == PhotoMark::Reject);
  // Turn the outgoing sidecar destination into a directory to exercise the
  // real failed-save prompt. Cancellation must preserve even a multi-selection.
  const auto sidecar = inputSidecarPath(last);
  CHECK(std::filesystem::remove(sidecar) && std::filesystem::create_directory(sidecar));
  app.filmstripSelection = {first, last};
  app.filmstripAnchor = first;
  const auto selection = app.filmstripSelection;
  bool prompted = false;
  CHECK(!ui::executeCommands(app, {Command::PreviousPhoto, Command::MarkPick}, [&](const std::string &) {
    prompted = true;
    return SaveFailureDecision::Cancel;
  }));
  CHECK(prompted && app.path == last && app.filmstripSelection == selection && app.filmstripAnchor == first);
  CHECK(app.currentPhotoMark == PhotoMark::Unmarked);
  return true;
}

bool shortcutsAndMenus() {
  for (const bool mac : {false, true}) {
    Files files; App app; Ui ui; ui.full = true;
    app.showRight = app.showNodes = app.showFilmstrip = false;
    auto &io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigMacOSXBehaviors = mac;
    CHECK(openPath(app, files.photo("keys.png")) && addNativeExposureNode(app));
    ui.tick(app); ui.tick(app);
    CHECK(canUndoGrade(app) && !canRedoGrade(app));
    ui.key(app, ImGuiKey_Z, true, true);
    CHECK(app.shortcutFailure.empty());
    ui.key(app, ImGuiKey_Z, false, true);
    CHECK(app.nodes.size() == 1 && !canUndoGrade(app) && canRedoGrade(app));
    app.setStatus("Unrelated preview status");
    CHECK(!ui::executeCommands(app, {ui::ApplicationCommand::Undo, ui::ApplicationCommand::Inspector}));
    CHECK(app.shortcutFailure == "Step 1 (Undo): Unavailable or cancelled" && !app.showRight);
    ui.key(app, ImGuiKey_Z, false, true);
    CHECK(app.shortcutFailure.empty());
    ui.key(app, ImGuiKey_C, false, true);
    CHECK(app.shortcutFailure.empty());
    ui.key(app, ImGuiKey_Z, true, true);
    CHECK(app.nodes.size() == 2 && canUndoGrade(app));
    ui.key(app, ImGuiKey_Z, false, true); ui.key(app, ImGuiKey_Y, false, true);
    CHECK(app.nodes.size() == (mac ? 1u : 2u));
    if (mac) ui.key(app, ImGuiKey_Z, true, true);
    ui.click(app, ui.textPoint); ImGui::GetIO().AddInputCharactersUTF8("abc"); ui.tick(app);
    ui.key(app, ImGuiKey_Z, false, true);
    CHECK(app.nodes.size() == 2); // ImGui text undo owns the chord.
    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_RightBracket);
    CHECK(!app.showRight);  // The application must protect its own chord, too.
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
    // A modal keeps ownership even when no text item is active.
    app.showAbout = true;
    ui.tick(app);
    CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
    ui.key(app, ImGuiKey_Z, false, true);
    CHECK(app.nodes.size() == 2);
    ImGui::ClosePopupToLevel(0, true);
    ui.tick(app);
    // Actual Edit menu disabled flags, including the platform shortcut text.
    ui.click(app, {65, 10});
    auto *menu = ImGui::FindWindowByName("##Menu_00"); CHECK(menu);
    CHECK(ui.log.find("Undo") != std::string::npos && ui.log.find(mac ? "⌘+Z" : "Ctrl+Z") != std::string::npos);
    const ImVec2 undo{menu->DC.CursorStartPos.x + 20, menu->DC.CursorStartPos.y + ImGui::GetTextLineHeight() / 2};
    const ImVec2 redo{undo.x, undo.y + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y};
    ui.click(app, redo); CHECK(app.nodes.size() == 2); // Disabled Redo.
    ui.click(app, undo); CHECK(app.nodes.size() == 1);
    ui.click(app, {65, 10}); ui.click(app, redo); CHECK(app.nodes.size() == 2);
    ui.click(app, {900, 600});
    app.keyboardBindings = ui::KeyboardBindings{{ImGuiKey_F8, {ui::ApplicationCommand::Undo}}};
    ui.click(app, {65, 10});
    CHECK(ui.log.find("Undo") != std::string::npos && ui.log.find("F8") != std::string::npos);
    CHECK(ui.log.find(mac ? "⌘+Z" : "Ctrl+Z") == std::string::npos);
    ui.click(app, {900, 600});
    ui.key(app, ImGuiKey_Z, false, true);
    CHECK(app.nodes.size() == 2);
    ui.key(app, ImGuiKey_F8);
    CHECK(app.nodes.size() == 1);
    CHECK(app.history.redo(app));
    app.keyboardBindings->clear();
    ui.click(app, {65, 10});
    CHECK(ui.log.find("Undo") != std::string::npos && ui.log.find("F8") == std::string::npos);
    ui.click(app, {900, 600});
    ui.key(app, ImGuiKey_F8);
    CHECK(app.nodes.size() == 2);
    app.keyboardBindings.reset();
    app.history.clear();
    ui.key(app, ImGuiKey_Z, false, true); CHECK(app.nodes.size() == 2 && !canUndoGrade(app));
  }
  return true;
}

bool clipboardAndPanelShortcuts() {
  for (const bool mac : {false, true}) {
    Files files;
    App app;
    Ui ui;
    ui.full = true;
    app.showRight = app.showNodes = app.showFilmstrip = false;
    auto &io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigMacOSXBehaviors = mac;
    CHECK(openPath(app, files.photo("clipboard-keys.png")) && addNativeExposureNode(app));
    ui.tick(app);
    ui.tick(app);

    PersistTransfer transfer;
    std::string error;
    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_C);
    CHECK(decodeTransferPayload(ui.clipboard, transfer, error) && std::holds_alternative<PersistFragment>(transfer));
    ui.chord(app, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C);
    CHECK(decodeTransferPayload(ui.clipboard, transfer, error) && std::holds_alternative<PersistGrade>(transfer));
    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_C);
    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_V);
    CHECK(app.nodes.size() == 3);

    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_RightBracket);
    CHECK(app.showRight);
    ui.chord(app, ImGuiMod_Super | ImGuiKey_RightBracket);
    CHECK(!app.showRight);
    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_Backslash);
    CHECK(app.showFilmstrip);
    ui.chord(app, ImGuiMod_Super | ImGuiKey_Backslash);
    CHECK(!app.showFilmstrip);

    ui.click(app, {65, 10});
    CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_RightBracket);
    CHECK(!app.showRight);
    ui.click(app, {900, 600});
    CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));

    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_RightBracket);
    ui.chord(app, ImGuiMod_Ctrl | ImGuiKey_F);
    io.AddInputCharactersUTF8("test");
    ui.tick(app);
    CHECK(std::string(app.paramFilter) == "test" && ImGui::IsAnyItemActive());
    app.keyboardBindings = ui::KeyboardBindings{{ImGuiKey_F8, {ui::ApplicationCommand::Filmstrip}}};

    // Escape ends the active text edit during DrawUiFrame. A second key from
    // that frame must still belong to the text interaction.
    ui.chordEvent(ImGuiKey_Escape, true);
    ui.chordEvent(ImGuiKey_F8, true);
    ui.tick(app);
    CHECK(!ImGui::IsAnyItemActive() && !app.showFilmstrip);
    ui.chordEvent(ImGuiKey_Escape, false);
    ui.chordEvent(ImGuiKey_F8, false);
    ui.tick(app);
    ui.chord(app, ImGuiKey_F8);
    CHECK(app.showFilmstrip);
  }
  return true;
}
}

bool testHistoryUi() {
  if (!radialInspectorBoolean() || !radialInspectorGestures() || !opacityGesturesAndPersistence() || !nativeGestures() || !ctlDeferredInputAndFailure() || !droppedPhotoCompletesOutgoingGesture() || !shortcutsAndMenus()) return false;
  if (!orderedNodeCommands() || !orderedPhotoCommands() || !clipboardAndPanelShortcuts()) return false;
  std::puts("ok  Ordered keyboard commands: parameter targeting, partial completion, per-edit Undo and Filmstrip navigation");
  std::puts("ok  History UI: native gestures, numeric commits/interruption, discrete buttons, shortcuts and eligibility");
  return true;
}
