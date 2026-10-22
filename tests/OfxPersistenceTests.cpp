#include "OfxPersistenceTests.h"

#include "GraphEvaluator.h"
#include "NodeGraph.h"
#include "ofx/OfxHost.h"
#include "processors/OfxProcessor.h"
#include "ui/UiContext.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "ofxParam.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "OFX persistence check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
namespace fs = std::filesystem;
struct Files {
  fs::path dir = fs::temp_directory_path() / ("rawnode-ofx-persistence-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { fs::create_directory(dir); }
  ~Files() { std::error_code ignored; fs::remove_all(dir, ignored); }
  std::string path(const char *name) const { return (dir / name).string(); }
};

// The real adapter/factory sees OFX descriptor properties, including defaults.
// Render reads current parameters, with an independently specified gain of 2*3.
struct Plugin {
  OfxPlugin plugin{};
  int index = int(gPlugins.size());
  Plugin() {
    plugin.pluginIdentifier = "org.rawnode.selftest.persistence";
    plugin.mainEntry = [](const char *action, const void *handle, OfxPropertySetHandle, OfxPropertySetHandle) {
      if (std::strcmp(action, kOfxImageEffectActionRender)) return kOfxStatReplyDefault;
      auto *effect = static_cast<Effect *>(const_cast<void *>(handle));
      double gain;
      {
        std::lock_guard<std::mutex> lock(gValueMutex);
        gain = findParam(effect, "visiblePersistent")->v[0] * findParam(effect, "hiddenPersistent")->v[0];
      }
      for (int i = 0; i < effect->w * effect->h * 4; ++i)
        effect->dst[i] = effect->src[i] * (i % 4 == 3 ? 1.0f : float(gain));
      return kOfxStatOK;
    };
    PluginEntry entry{};
    entry.plugin = &plugin;
    entry.label = "Persistence fixture";
    entry.descriptor = std::make_unique<Effect>();
    for (bool hidden : {false, true}) for (bool persistent : {false, true}) {
      const std::string id = std::string(hidden ? "hidden" : "visible") + (persistent ? "Persistent" : "Transient");
      auto param = std::make_unique<Param>();
      param->name = id;
      param->type = kOfxParamTypeDouble;
      propSetString(H(&param->props), kOfxPropLabel, 0, id.c_str());
      propSetInt(H(&param->props), kOfxParamPropSecret, 0, hidden);
      propSetInt(H(&param->props), kOfxParamPropPersistant, 0, persistent);
      propSetDouble(H(&param->props), kOfxParamPropDefault, 0, 1.0);
      entry.descriptor->params.push_back(std::move(param));
    }
    // A hidden custom value also exercises the private-data string representation
    // and omitted Persistant property (the OFX default is true).
    auto custom = std::make_unique<Param>();
    custom->name = "privateText";
    custom->type = kOfxParamTypeCustom;
    propSetInt(H(&custom->props), kOfxParamPropSecret, 0, 1);
    entry.descriptor->params.push_back(std::move(custom));
    gPlugins.push_back(std::move(entry));
  }
  ~Plugin() { gPlugins.pop_back(); } // Apps/instances must die before the descriptor.
};

// Use the application's real clipboard callbacks and real Inspector drawing,
// without a window system or GL context.
struct Ui {
  ImGuiContext *previous = ImGui::GetCurrentContext();
  ImGuiContext *context = ImGui::CreateContext();
  std::string clipboard;
  Ui() {
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto &platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = this;
    platform.Platform_GetClipboardTextFn = [](ImGuiContext *) {
      return static_cast<Ui *>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->clipboard.c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *text) {
      static_cast<Ui *>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->clipboard = text;
    };
  }
  ~Ui() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
  std::string inspector(App &app, const char *filter) {
    std::snprintf(app.paramFilter, sizeof app.paramFilter, "%s", filter);
    auto &io = ImGui::GetIO(); io.DisplaySize = {800, 800}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({600, 700});
    ImGui::Begin("OFX persistence Inspector");
    ImGui::LogToBuffer(0);
    drawInspector(app);
    const std::string text = context->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::EndFrame();
    return text;
  }
};

const std::string privateText = "analysis \"state\"\n\u03bb";
const std::string unknownJson = R"({"integer":9007199254740993,"curve":[0,0.12345678901234567890123456789]})";
ProcessorParameter parameter(const Node &node, const char *id) {
  for (const auto &param : node.processor->parameters()) if (param.id == id) return param;
  throw std::runtime_error(std::string("Missing fixture parameter: ") + id);
}
const PersistNode &record(const PersistGraph &graph, const NodeId &id) {
  for (const auto &node : graph.nodes) if (node.id == id.value) return node;
  throw std::runtime_error("Missing fixture node");
}
bool savedValues(const PersistGraph &graph, const NodeId &id) {
  const auto &params = record(graph, id).paramsJson;
  CHECK(params.size() == 4 && params.at("visiblePersistent") == "2" && params.at("hiddenPersistent") == "3");
  CHECK(params.count("privateText") && params.at("future") == unknownJson);
  CHECK(!params.count("visibleTransient") && !params.count("hiddenTransient"));
  return true;
}
bool restoredValues(const Node &node) {
  CHECK(node.processor && node.processor->backend() == ProcessorBackend::OFX);
  CHECK(parameter(node, "visiblePersistent").value == ParameterValue(2.0));
  CHECK(parameter(node, "hiddenPersistent").value == ParameterValue(3.0));
  CHECK(parameter(node, "visibleTransient").value == ParameterValue(1.0));
  CHECK(parameter(node, "hiddenTransient").value == ParameterValue(1.0));
  CHECK(parameter(node, "privateText").value == ParameterValue(privateText));
  CHECK(node.preservedParamsJson.at("future") == unknownJson);
  for (bool hidden : {false, true}) for (bool persistent : {false, true}) {
    const auto id = std::string(hidden ? "hidden" : "visible") + (persistent ? "Persistent" : "Transient");
    const auto param = parameter(node, id.c_str());
    CHECK(param.secret == hidden && param.persistValue == persistent);
  }
  CHECK(parameter(node, "privateText").secret && parameter(node, "privateText").persistValue);
  return true;
}
bool pixels(const App &app) {
  // Hand-calculated result: RGB gain 6, alpha unchanged. Negative and HDR
  // samples deliberately distinguish persistence correctness from clipping.
  auto input = std::make_shared<Image>();
  input->w = input->h = 1; input->px = {0.25f, -0.5f, 2.0f, 0.375f};
  const auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                   {EvaluationPurpose::Preview, 1, 1, 1, 1}, input);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
  const auto *image = std::get_if<ImageValue>(&result.value);
  CHECK(image && *image && (*image)->w == 1 && (*image)->h == 1);
  CHECK((*image)->px == (std::vector<float>{1.5f, -3.0f, 12.0f, 0.375f}));
  // Also process the document's actual decoded photograph after application or
  // reopen. Its independently expected operation is the same RGB gain of 6.
  auto photo = std::make_shared<Image>(app.full);
  CHECK(photo->w == 1 && photo->h == 1 && photo->px.size() == 4);
  const auto processed = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                      {EvaluationPurpose::Preview, 1, 1, 1, 1}, photo);
  CHECK(processed.state == EvaluationState::Available && processed.diagnostics.empty());
  const auto *actual = std::get_if<ImageValue>(&processed.value);
  CHECK(actual && *actual && (*actual)->px.size() == 4);
  for (size_t i = 0; i < 4; ++i)
    CHECK((*actual)->px[i] == photo->px[i] * (i == 3 ? 1.0f : 6.0f));
  return true;
}

bool productionRoundTrips(Plugin &plugin) {
  Files files; Ui ui;
  const auto photo = files.path("photo.png"), preset = files.path("preset.rawnode.json");
  Image input; input.w = input.h = 1; input.px = {0.25f, 0.5f, 0.75f, 1.0f};
  CHECK(writeImage(input, photo, {RgbGamut::Rec709, TransferFunction::Linear}));
  const auto openDestination = [&](App &destination, const char *name) {
    const auto path = files.path(name);
    return writeImage(input, path, {RgbGamut::Rec709, TransferFunction::Linear}) && openPath(destination, path);
  };
  App app;
  CHECK(openPath(app, photo) && addNode(app, plugin.index));
  const auto id = *app.selectedNodeId();
  auto &node = *findNode(app, id);
  CHECK(setNodeParameterValue(app, id, "visiblePersistent", 2.0));
  CHECK(setNodeParameterValue(app, id, "hiddenPersistent", 3.0));
  CHECK(setNodeParameterValue(app, id, "visibleTransient", 17.0));
  CHECK(setNodeParameterValue(app, id, "hiddenTransient", 19.0));
  CHECK(node.processor->setParameterValue("privateText", privateText));
  node.preservedParamsJson["future"] = unknownJson;
  // Now-known transient data must be erased, while unknown data survives.
  node.preservedParamsJson["visibleTransient"] = "23";
  node.preservedParamsJson["hiddenTransient"] = "29";
  CHECK(pixels(app));

  CHECK(saveCurrentInputSidecar(app));
  PersistFile saved; std::string error;
  CHECK(readGraphSidecar(inputSidecarPath(photo), saved, error) == SidecarReadStatus::Loaded && saved.grade);
  CHECK(savedValues(saved.grade->graph, id));
  // A new App proves restoration from disk, without reusing live instances.
  App reopened;
  CHECK(openPath(reopened, photo) && !reopened.isPhotoProtected());
  CHECK(restoredValues(*findNode(reopened, id)) && pixels(reopened));
  CHECK(selectNode(reopened, id));
  const auto log = ui.inspector(reopened, "");
  CHECK(log.find("visiblePersistent") != std::string::npos && log.find("visibleTransient") != std::string::npos);
  CHECK(log.find("hiddenPersistent") == std::string::npos && log.find("hiddenTransient") == std::string::npos);
  const auto filtered = ui.inspector(reopened, "hidden");
  CHECK(filtered.find("hiddenPersistent") == std::string::npos && filtered.find("hiddenTransient") == std::string::npos);
  CHECK(openPath(reopened, photo)); // Production same-photo save-before-reopen.
  CHECK(restoredValues(*findNode(reopened, id)) && pixels(reopened));

  CHECK(savePreset(app, preset));
  PersistGrade grade;
  CHECK(loadPresetFile(preset, grade, error) && savedValues(grade.graph, id));
  App replaced, appended;
  CHECK(openDestination(replaced, "replace.png") && loadPresetDialog(replaced, false, preset));
  CHECK(restoredValues(*findNode(replaced, id)) && pixels(replaced));
  CHECK(openDestination(appended, "append.png") && loadPresetDialog(appended, true, preset));
  CHECK(appended.documentOutput.nodeId != id);
  CHECK(restoredValues(*findNode(appended, appended.documentOutput.nodeId)) && pixels(appended));

  CHECK(copyGrade(app));
  PersistTransfer transfer;
  CHECK(decodeTransferPayload(ui.clipboard, transfer, error));
  CHECK(savedValues(std::get<PersistGrade>(transfer).graph, id));
  App gradePaste;
  CHECK(openDestination(gradePaste, "grade-paste.png") && pasteFromClipboard(gradePaste));
  CHECK(restoredValues(*findNode(gradePaste, id)) && pixels(gradePaste));
  CHECK(copySelectedNode(app));
  CHECK(decodeTransferPayload(ui.clipboard, transfer, error));
  const auto &fragment = std::get<PersistFragment>(transfer);
  CHECK(fragment.nodes.size() == 1 && fragment.nodes[0].paramsJson == record(grade.graph, id).paramsJson);
  App nodePaste;
  CHECK(openDestination(nodePaste, "node-paste.png") && pasteFromClipboard(nodePaste));
  CHECK(nodePaste.documentOutput.nodeId != id);
  CHECK(restoredValues(*findNode(nodePaste, nodePaste.documentOutput.nodeId)) && pixels(nodePaste));
  std::puts("ok  OFX persistence: four visibility/persistence combinations, real grade save/reopen, preset Replace/Append, grade/node clipboard and expected pixels");
  std::puts("ok  OFX Inspector: hidden persistent and transient parameters stay hidden, including search");
  return true;
}

bool nonpersistentRestoreGuards(Plugin &plugin) {
  Files files; App app;
  CHECK(addNode(app, plugin.index));
  const auto photo = files.path("protected.png");
  Image input; input.w = input.h = 1; input.px = {0.25f, 0.5f, 0.75f, 1.0f};
  CHECK(writeImage(input, photo, {RgbGamut::Rec709, TransferFunction::Linear}));
  PersistGrade grade;
  CHECK(captureGrade(app, grade));
  const auto id = *app.selectedNodeId();
  // The graph-native contract rejects known non-persistent saved values;
  // it must not silently apply or drop them as an old-file compatibility path.
  for (const char *name : {"visibleTransient", "hiddenTransient"}) {
    auto invalid = grade;
    for (auto &node : invalid.graph.nodes) if (node.id == id.value) node.paramsJson[name] = "7";
    std::string json, error;
    PersistFile file; file.grade = invalid;
    CHECK(encodeGraphFile(file, json, error)); // Codec has no installed metadata.
    const auto path = files.path(name);
    { std::ofstream out(path); out << json; CHECK(out.good()); }
    App destination;
    CHECK(addNativeExposureNode(destination));
    const auto original = destination.documentOutput;
    auto *processor = findNode(destination, original.nodeId)->processor.get();
    CHECK(!loadPresetDialog(destination, false, path));
    CHECK(destination.documentOutput == original && findNode(destination, original.nodeId)->processor.get() == processor);
    CHECK(destination.getStatus().find(std::string("Non-persistent installed parameter: ") + name) != std::string::npos);

    const auto sidecar = inputSidecarPath(photo);
    { std::ofstream out(sidecar); out << json; CHECK(out.good()); }
    App protectedPhoto;
    CHECK(openPath(protectedPhoto, photo) && protectedPhoto.isPhotoProtected());
    CHECK(protectedPhoto.persistenceWarning.find(std::string("Non-persistent installed parameter: ") + name) != std::string::npos);
    CHECK(!saveCurrentInputSidecar(protectedPhoto));
    std::ifstream in(sidecar);
    CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == json);
  }
  std::puts("ok  OFX non-persistent values: installed transient restore refusal preserves the destination");
  return true;
}
}  // namespace

bool testOfxPersistence() {
  Plugin plugin;
  return productionRoundTrips(plugin) && nonpersistentRestoreGuards(plugin);
}
