#include "OfxPersistenceTests.h"

#include "GraphEvaluator.h"
#include "DocumentMutation.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "ofx/OfxHost.h"
#include "ofx/OfxHostPriv.h"
#include "processors/OfxProcessor.h"
#include "ui/UiContext.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "ofxParam.h"
#include "ofxProperty.h"

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

template <class Suite>
const Suite &suite(const char *name) {
  return *static_cast<const Suite *>(gOfxHost.fetchSuite(gOfxHost.host, name, 1));
}
const auto &properties() { return suite<OfxPropertySuiteV1>(kOfxPropertySuite); }
const auto &effects() { return suite<OfxImageEffectSuiteV1>(kOfxImageEffectSuite); }
const auto &parameters() { return suite<OfxParameterSuiteV1>(kOfxParameterSuite); }

OfxParamHandle paramHandle(OfxParamSetHandle params, const char *name) {
  OfxParamHandle param = nullptr;
  parameters().paramGetHandle(params, name, &param, nullptr);
  return param;
}

// The plugin reads through the real OFX suites during CreateInstance and renders
// from private cached state. Merely restoring the visible values cannot pass.
struct Plugin {
  struct Cache {
    double visibleAtCreate = 0, hiddenAtCreate = 0, gain = 0;
    int notifications = 0;
  };
  OfxPlugin plugin{};
  int index = int(gPlugins.size());
  int creates = 0, destroys = 0, live = 0, invalidDestroys = 0, preCreationNotifications = 0;
  OfxStatus createStatus = kOfxStatOK;
  bool adjustGainOnCreate = false;
  std::function<void(OfxParamSetHandle)> inspectInitialValues;
  Plugin() {
    plugin.pluginIdentifier = "org.rawnode.selftest.persistence";
    plugin.mainEntry = [](const char *action, const void *handle, OfxPropertySetHandle, OfxPropertySetHandle) {
      const auto effectHandle = static_cast<OfxImageEffectHandle>(const_cast<void *>(handle));
      OfxPropertySetHandle props = nullptr;
      effects().getPropertySet(effectHandle, &props);
      void *probe = nullptr, *data = nullptr;
      properties().propGetPointer(props, "selftest-probe", 0, &probe);
      properties().propGetPointer(props, kOfxPropInstanceData, 0, &data);
      auto &self = *static_cast<Plugin *>(probe);
      auto *cache = static_cast<Cache *>(data);
      OfxParamSetHandle params = nullptr;
      effects().getParamSet(effectHandle, &params);
      const auto readGain = [&] {
        double visible = 0, hidden = 0;
        parameters().paramGetValue(paramHandle(params, "visiblePersistent"), &visible);
        parameters().paramGetValue(paramHandle(params, "hiddenPersistent"), &hidden);
        return visible * hidden;
      };
      if (!std::strcmp(action, kOfxActionCreateInstance)) {
        ++self.creates;
        if (self.inspectInitialValues) self.inspectInitialValues(params);
        if (self.createStatus != kOfxStatOK && self.createStatus != kOfxStatReplyDefault) return self.createStatus;
        auto state = std::make_unique<Cache>();
        parameters().paramGetValue(paramHandle(params, "visiblePersistent"), &state->visibleAtCreate);
        parameters().paramGetValue(paramHandle(params, "hiddenPersistent"), &state->hiddenAtCreate);
        state->gain = readGain();
        properties().propSetPointer(props, kOfxPropInstanceData, 0, state.release());
        ++self.live;
        // An intentional plugin edit detects accidental second restoration.
        if (self.adjustGainOnCreate) parameters().paramSetValue(paramHandle(params, "visiblePersistent"), 6.0);
        return self.createStatus;
      }
      if (!std::strcmp(action, kOfxActionDestroyInstance)) {
        ++self.destroys;
        if (!cache) { ++self.invalidDestroys; return kOfxStatFailed; }
        delete cache;
        --self.live;
        properties().propSetPointer(props, kOfxPropInstanceData, 0, nullptr);
        return kOfxStatOK;
      }
      if (!std::strcmp(action, kOfxActionBeginInstanceChanged) ||
          !std::strcmp(action, kOfxActionInstanceChanged) ||
          !std::strcmp(action, kOfxActionEndInstanceChanged)) {
        if (!cache) { ++self.preCreationNotifications; return kOfxStatFailed; }
        ++cache->notifications;
        cache->gain = readGain(); // Ordinary UI edits still update private state.
        return kOfxStatOK;
      }
      if (std::strcmp(action, kOfxImageEffectActionRender)) return kOfxStatReplyDefault;
      if (!cache) return kOfxStatFailed;
      auto *effect = static_cast<Effect *>(const_cast<void *>(handle));
      for (int i = 0; i < effect->w * effect->h * 4; ++i)
        effect->dst[i] = effect->src[i] * (i % 4 == 3 ? 1.0f : float(cache->gain));
      return kOfxStatOK;
    };
    PluginEntry entry{};
    entry.plugin = &plugin;
    entry.label = "Persistence fixture";
    entry.descriptor = std::make_unique<Effect>();
    propSetPointer(H(&entry.descriptor->props), "selftest-probe", 0, this);
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
const Plugin::Cache &cache(const Node &node) {
  const auto *ofx = static_cast<const OfxProcessor *>(node.processor.get());
  return *static_cast<Plugin::Cache *>(ofx->effect()->props.m.at(kOfxPropInstanceData)[0].p);
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
  CHECK(cache(node).visibleAtCreate == 2.0 && cache(node).hiddenAtCreate == 3.0);
  CHECK(cache(node).gain == 6.0 && cache(node).notifications == 0);
  CHECK(node.preservedParamsJson.at("future") == unknownJson);
  for (bool hidden : {false, true}) for (bool persistent : {false, true}) {
    const auto id = std::string(hidden ? "hidden" : "visible") + (persistent ? "Persistent" : "Transient");
    const auto param = parameter(node, id.c_str());
    CHECK(param.secret == hidden && param.persistValue == persistent);
  }
  CHECK(parameter(node, "privateText").secret && parameter(node, "privateText").persistValue);
  return true;
}
bool pixels(const App &app, float gain = 6.0f) {
  // Independently specified RGB gain, alpha unchanged. Negative and HDR
  // samples deliberately distinguish persistence correctness from clipping.
  auto input = std::make_shared<Image>();
  input->w = input->h = 1; input->px = {0.25f, -0.5f, 2.0f, 0.375f};
  const auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                   {EvaluationPurpose::Preview, 1, 1, 1, 1}, input);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
  const auto *image = std::get_if<ImageValue>(&result.value);
  CHECK(image && *image && (*image)->w == 1 && (*image)->h == 1);
  CHECK((*image)->px == (std::vector<float>{0.25f * gain, -0.5f * gain, 2.0f * gain, 0.375f}));
  // Also process the document's actual decoded photograph after application or
  // reopen. Its independently expected operation is the same RGB gain.
  auto photo = std::make_shared<Image>(app.full);
  CHECK(photo->w == 1 && photo->h == 1 && photo->px.size() == 4);
  const auto processed = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, app.documentOutput,
                                      {EvaluationPurpose::Preview, 1, 1, 1, 1}, photo);
  CHECK(processed.state == EvaluationState::Available && processed.diagnostics.empty());
  const auto *actual = std::get_if<ImageValue>(&processed.value);
  CHECK(actual && *actual && (*actual)->px.size() == 4);
  for (size_t i = 0; i < 4; ++i)
    CHECK((*actual)->px[i] == photo->px[i] * (i == 3 ? 1.0f : gain));
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

bool cachedGainFiveReopen(Plugin &plugin) {
  Files files;
  const auto photo = files.path("cached-gain.png");
  Image input; input.w = input.h = 1; input.px = {0.25f, 0.5f, 0.75f, 1.0f};
  CHECK(writeImage(input, photo, {RgbGamut::Rec709, TransferFunction::Linear}));
  App original;
  CHECK(openPath(original, photo) && addNode(original, plugin.index));
  const auto id = *original.selectedNodeId();
  CHECK(parameter(*findNode(original, id), "visiblePersistent").value == ParameterValue(1.0));
  CHECK(cache(*findNode(original, id)).visibleAtCreate == 1.0);
  CHECK(setNodeParameterValue(original, id, "visiblePersistent", 5.0));
  CHECK(saveCurrentInputSidecar(original));
  const int creates = plugin.creates;
  App reopened;
  CHECK(openPath(reopened, photo) && !reopened.isPhotoProtected());
  CHECK(plugin.creates == creates + 1);
  const auto &node = *findNode(reopened, id);
  CHECK(parameter(node, "visiblePersistent").value == ParameterValue(5.0));
  CHECK(cache(node).visibleAtCreate == 5.0 && cache(node).gain == 5.0);
  CHECK(cache(node).notifications == 0);
  auto samples = std::make_shared<Image>();
  samples->w = samples->h = 1; samples->px = {0.25f, -0.5f, 2.0f, 0.375f};
  const auto result = evaluateGraph({reopened.nodes, reopened.primarySource, reopened.documentOutput}, reopened.documentOutput,
                                   {EvaluationPurpose::Preview, 1, 1, 1, 1}, samples);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
  const auto *image = std::get_if<ImageValue>(&result.value);
  // Exact binary fractions times integer gain 5 need no numerical tolerance.
  CHECK(image && *image && (*image)->px == (std::vector<float>{1.25f, -2.5f, 10.0f, 0.375f}));
  std::puts("ok  OFX initial values: saved gain 5 read by the real parameter suite during CreateInstance, cached gain 5, exact pixels, no change notifications");
  return true;
}

bool initialValueValidationAndLifecycle(Plugin &plugin) {
  App app;
  CHECK(addNode(app, plugin.index));
  const auto id = *app.selectedNodeId();
  CHECK(setNodeParameterValue(app, id, "visiblePersistent", 5.0));
  auto graph = captureGraph({app.nodes, app.primarySource, app.documentOutput});
  for (auto &node : graph.nodes) if (node.id == id.value) {
    node.paramsJson.erase("hiddenPersistent"); // Missing persistent values keep defaults.
    node.paramsJson.erase("privateText");
  }
  {
    auto restored = document_detail::restoreGraph(graph);
    const auto &node = restored.nodes.back();
    CHECK(parameter(node, "hiddenPersistent").value == ParameterValue(1.0));
    CHECK(parameter(node, "privateText").value == ParameterValue(std::string()));
    CHECK(cache(node).visibleAtCreate == 5.0 && cache(node).hiddenAtCreate == 1.0);
    CHECK(cache(node).notifications == 0);
  }
  // The plugin may change an initial value itself. Never restore over it again.
  plugin.adjustGainOnCreate = true;
  {
    auto restored = document_detail::restoreGraph(graph);
    const auto &node = restored.nodes.back();
    CHECK(cache(node).visibleAtCreate == 5.0 && cache(node).gain == 5.0);
    CHECK(parameter(node, "visiblePersistent").value == ParameterValue(6.0));
    CHECK(cache(node).notifications == 0);
  }
  plugin.adjustGainOnCreate = false;
  // Unwinding a later invalid node destroys only the successfully created one.
  {
    auto partial = graph;
    auto invalid = graph.nodes.back();
    invalid.id = "invalid-initial-values";
    invalid.paramsJson["visiblePersistent"] = "false";
    partial.nodes.push_back(std::move(invalid));
    const int creates = plugin.creates, destroys = plugin.destroys, live = plugin.live;
    bool rejected = false;
    try { (void)document_detail::restoreGraph(partial); }
    catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected && plugin.creates == creates + 1 && plugin.destroys == destroys + 1 && plugin.live == live);
  }
  // Reject invalid installed values before any plugin lifecycle action.
  const int creates = plugin.creates, destroys = plugin.destroys, live = plugin.live;
  for (const auto &invalid : {"\"5\"", "1e999", "[5]"}) {
    auto bad = graph;
    for (auto &node : bad.nodes) if (node.id == id.value) node.paramsJson["visiblePersistent"] = invalid;
    bool rejected = false;
    try { (void)document_detail::restoreGraph(bad); }
    catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected && plugin.creates == creates && plugin.destroys == destroys && plugin.live == live);
  }
  bool threw = false;
  try {
    (void)OfxProcessor::create(plugin.index, [](Processor &prepared) {
      prepared.setParameterValue("visiblePersistent", 5.0); // Even notify=true is silent until created.
      throw std::runtime_error("Abort parameter preparation");
    });
  } catch (const std::runtime_error &) { threw = true; }
  CHECK(threw && plugin.creates == creates && plugin.destroys == destroys && plugin.live == live);
  CHECK(plugin.preCreationNotifications == 0);
  plugin.createStatus = kOfxStatFailed;
  CHECK(!OfxProcessor::create(plugin.index));
  CHECK(plugin.creates == creates + 1 && plugin.destroys == destroys && plugin.live == live);
  const auto original = app.documentOutput;
  auto *processor = findNode(app, id)->processor.get();
  CHECK(!addNode(app, plugin.index));
  CHECK(app.documentOutput == original && findNode(app, id)->processor.get() == processor);
  {
    auto restored = document_detail::restoreGraph(graph);
    const auto &missing = restored.nodes.back();
    CHECK(missing.isMissingProcessor() && missing.preservedParamsJson.at("visiblePersistent") == "5");
  }
  CHECK(plugin.destroys == destroys && plugin.live == live && plugin.invalidDestroys == 0);
  plugin.createStatus = kOfxStatReplyDefault;
  {
    auto prepared = OfxProcessor::create(plugin.index, [](Processor &processor) {
      processor.setParameterValue("visiblePersistent", 5.0);
    });
    CHECK(prepared);
    auto *state = static_cast<Plugin::Cache *>(prepared->effect()->props.m.at(kOfxPropInstanceData)[0].p);
    CHECK(state->visibleAtCreate == 5.0 && state->notifications == 0);
    CHECK(plugin.live == live + 1);
  }
  CHECK(plugin.destroys == destroys + 1 && plugin.live == live && plugin.invalidDestroys == 0);
  CHECK(plugin.preCreationNotifications == 0);
  plugin.createStatus = kOfxStatOK;
  std::puts("ok  OFX construction: missing defaults, no second restore, silent preparation, validation aborts, failed creation, missing processor retention and balanced destruction");
  std::puts("ok  OFX initialisation: no BeginInstanceChanged, InstanceChanged or EndInstanceChanged before successful creation, including notify=true preparation");
  return true;
}

bool dynamicChoiceRestoration() {
  Plugin plugin;
  plugin.plugin.pluginIdentifier = "org.rawnode.selftest.dynamic-choice";
  Files files;
  const auto photo = files.path("dynamic-choice.png"), preset = files.path("choice.rawnode.json");
  OfxParamSetHandle descriptor = nullptr;
  effects().getParamSet(reinterpret_cast<OfxImageEffectHandle>(gPlugins[plugin.index].descriptor.get()), &descriptor);
  OfxPropertySetHandle descriptorOptions = nullptr;
  CHECK(parameters().paramDefine(descriptor, kOfxParamTypeChoice, "dynamicChoice", &descriptorOptions) == kOfxStatOK);
  int optionCount = -1;
  CHECK(properties().propGetDimension(descriptorOptions, kOfxParamPropChoiceOption, &optionCount) == kOfxStatOK && optionCount == 0);

  int observed = -1, replacement = -1, populateCount = 3;
  bool suiteOK = true;
  plugin.inspectInitialValues = [&](OfxParamSetHandle params) {
    OfxParamHandle choice = nullptr;
    OfxPropertySetHandle options = nullptr;
    suiteOK = parameters().paramGetHandle(params, "dynamicChoice", &choice, &options) == kOfxStatOK &&
              parameters().paramGetValue(choice, &observed) == kOfxStatOK;
    const char *labels[] = {"First", "Second", "Third"};
    for (int i = 0; i < populateCount; ++i)
      suiteOK &= properties().propSetString(options, kOfxParamPropChoiceOption, i, labels[i]) == kOfxStatOK;
    if (replacement >= 0) suiteOK &= parameters().paramSetValue(choice, replacement) == kOfxStatOK;
  };

  Image input; input.w = input.h = 1; input.px = {0.25f, 0.5f, 0.75f, 1.0f};
  CHECK(writeImage(input, photo, {RgbGamut::Rec709, TransferFunction::Linear}));
  App original;
  CHECK(openPath(original, photo) && addNode(original, plugin.index) && suiteOK && observed == 0);
  const auto id = *original.selectedNodeId();
  CHECK(setNodeParameterValue(original, id, "dynamicChoice", 2));
  findNode(original, id)->preservedParamsJson["future"] = unknownJson;
  CHECK(saveCurrentInputSidecar(original));
  PersistFile saved; std::string error;
  CHECK(readGraphSidecar(inputSidecarPath(photo), saved, error) == SidecarReadStatus::Loaded && saved.grade);
  CHECK(record(saved.grade->graph, id).paramsJson.at("dynamicChoice") == "2");
  {
    const int creates = plugin.creates;
    App reopened;
    CHECK(openPath(reopened, photo) && !reopened.isPhotoProtected());
    CHECK(suiteOK && observed == 2 && plugin.creates == creates + 1);
    const auto &node = *findNode(reopened, id);
    const auto choice = parameter(node, "dynamicChoice");
    CHECK(choice.value == ParameterValue(2) && choice.choices == (std::vector<std::string>{"First", "Second", "Third"}));
    CHECK(node.preservedParamsJson.at("future") == unknownJson && cache(node).notifications == 0);
    CHECK(properties().propGetDimension(descriptorOptions, kOfxParamPropChoiceOption, &optionCount) == kOfxStatOK && optionCount == 0);
  }
  // Menus may also append to a partial descriptor list during creation.
  properties().propSetString(descriptorOptions, kOfxParamPropChoiceOption, 0, "First");
  {
    auto restored = document_detail::restoreGraph(saved.grade->graph);
    CHECK(suiteOK && observed == 2 && parameter(restored.nodes.back(), "dynamicChoice").choices.size() == 3);
  }
  properties().propReset(descriptorOptions, kOfxParamPropChoiceOption);
  replacement = 1;
  {
    auto restored = document_detail::restoreGraph(saved.grade->graph);
    CHECK(suiteOK && observed == 2 && parameter(restored.nodes.back(), "dynamicChoice").value == ParameterValue(1));
    CHECK(record(captureGraph({restored.nodes, restored.primarySource, restored.output}), id).paramsJson.at("dynamicChoice") == "1");
  }
  // Invalid plugin results and a still-empty instance menu cannot be published.
  for (bool empty : {false, true}) {
    populateCount = empty ? 0 : 3;
    replacement = empty ? -1 : 3;
    const int creates = plugin.creates, destroys = plugin.destroys, live = plugin.live;
    bool rejected = false;
    try { (void)document_detail::restoreGraph(saved.grade->graph); }
    catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected && plugin.creates == creates + 1 && plugin.destroys == destroys + 1 && plugin.live == live);
  }
  populateCount = 3;
  replacement = -1;
  CHECK(savePreset(original, preset));
  App destination;
  const auto destinationPhoto = files.path("destination.png");
  CHECK(writeImage(input, destinationPhoto, {RgbGamut::Rec709, TransferFunction::Linear}));
  CHECK(openPath(destination, destinationPhoto) && addNativeExposureNode(destination));
  CHECK(saveCurrentInputSidecar(destination));
  const auto output = destination.documentOutput;
  auto *processor = findNode(destination, output.nodeId)->processor.get();
  const auto readText = [](const std::string &path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  const auto destinationSidecar = inputSidecarPath(destinationPhoto);
  const auto destinationBytes = readText(destinationSidecar);
  CHECK(!destinationBytes.empty());
  // Even a plugin that replaces an invalid saved index with 0 must not hide it.
  replacement = 0;
  for (const char *value : {"3", "-1", "1.5", "\"2\"", "true", "2147483648", "\"Third\""}) {
    auto invalid = saved;
    for (auto &node : invalid.grade->graph.nodes) if (node.id == id.value) node.paramsJson["dynamicChoice"] = value;
    std::string json;
    CHECK(encodeGraphFile(invalid, json, error));
    { std::ofstream out(preset); out << json; CHECK(out.good()); }
    const int creates = plugin.creates, destroys = plugin.destroys, live = plugin.live;
    CHECK(!loadPresetDialog(destination, false, preset));
    CHECK(destination.documentOutput == output && findNode(destination, output.nodeId)->processor.get() == processor);
    CHECK(readText(destinationSidecar) == destinationBytes);
    const int created = std::strcmp(value, "3") == 0 ? 1 : 0;
    CHECK(plugin.creates == creates + created && plugin.destroys == destroys + created && plugin.live == live);
    const auto sidecar = inputSidecarPath(photo);
    { std::ofstream out(sidecar); out << json; CHECK(out.good()); }
    App protectedPhoto;
    CHECK(openPath(protectedPhoto, photo) && protectedPhoto.isPhotoProtected());
    CHECK(protectedPhoto.persistenceWarning.find("Invalid parameter value: dynamicChoice") != std::string::npos);
    CHECK(!saveCurrentInputSidecar(protectedPhoto) && readText(sidecar) == json);
    CHECK(plugin.creates == creates + 2 * created && plugin.destroys == destroys + 2 * created && plugin.live == live);
  }
  replacement = -1;
  const auto validPreset = files.path("valid-choice.rawnode.json");
  CHECK(savePreset(original, validPreset) && loadPresetDialog(destination, false, validPreset));
  CHECK(suiteOK && observed == 2 && parameter(*findNode(destination, id), "dynamicChoice").value == ParameterValue(2));
  CHECK(plugin.preCreationNotifications == 0 && plugin.invalidDestroys == 0);
  std::puts("ok  OFX dynamic choices: saved index 2 available during creation, instance menus, plugin edits, strict rejection, balanced post-create cleanup and unchanged protected sidecars/preset destinations");
  return true;
}

bool typedInitialValues(Plugin &plugin) {
  OfxParamSetHandle descriptor = nullptr;
  effects().getParamSet(reinterpret_cast<OfxImageEffectHandle>(gPlugins[plugin.index].descriptor.get()), &descriptor);
  for (const auto &definition : std::vector<std::pair<const char *, const char *>>{
           {"integer", kOfxParamTypeInteger}, {"boolean", kOfxParamTypeBoolean},
           {"choice", kOfxParamTypeChoice}, {"text", kOfxParamTypeString},
           {"vector", kOfxParamTypeDouble3D}, {"intVector", kOfxParamTypeInteger2D}}) {
    OfxPropertySetHandle props = nullptr;
    CHECK(parameters().paramDefine(descriptor, definition.second, definition.first, &props) == kOfxStatOK);
    if (std::strcmp(definition.first, "choice") == 0) {
      properties().propSetString(props, kOfxParamPropChoiceOption, 0, "First");
      properties().propSetString(props, kOfxParamPropChoiceOption, 1, "Second");
    }
  }
  App app;
  CHECK(addNode(app, plugin.index));
  const auto id = *app.selectedNodeId();
  auto graph = captureGraph({app.nodes, app.primarySource, app.documentOutput});
  for (auto &node : graph.nodes) if (node.id == id.value) {
    node.paramsJson["integer"] = "-7";
    node.paramsJson["boolean"] = "true";
    node.paramsJson["choice"] = "1";
    node.paramsJson["text"] = jsonStringValue(privateText);
    node.paramsJson["privateText"] = jsonStringValue(privateText);
    node.paramsJson["vector"] = "[-0.5,0.25,2]";
    node.paramsJson["intVector"] = "[-3,4]";
  }
  bool observed = false;
  plugin.inspectInitialValues = [&](OfxParamSetHandle params) {
    int integer = 0, boolean = 0, choice = 0, x = 0, y = 0;
    double r = 0, g = 0, b = 0;
    char *text = nullptr, *custom = nullptr;
    observed = parameters().paramGetValue(paramHandle(params, "integer"), &integer) == kOfxStatOK && integer == -7 &&
        parameters().paramGetValue(paramHandle(params, "boolean"), &boolean) == kOfxStatOK && boolean == 1 &&
        parameters().paramGetValue(paramHandle(params, "choice"), &choice) == kOfxStatOK && choice == 1 &&
        parameters().paramGetValue(paramHandle(params, "vector"), &r, &g, &b) == kOfxStatOK && r == -0.5 && g == 0.25 && b == 2 &&
        parameters().paramGetValue(paramHandle(params, "intVector"), &x, &y) == kOfxStatOK && x == -3 && y == 4 &&
        parameters().paramGetValue(paramHandle(params, "text"), &text) == kOfxStatOK && text && text == privateText &&
        parameters().paramGetValue(paramHandle(params, "privateText"), &custom) == kOfxStatOK && custom && custom == privateText;
  };
  {
    auto restored = document_detail::restoreGraph(graph);
    CHECK(observed);
    const auto captured = captureGraph({restored.nodes, restored.primarySource, restored.output});
    CHECK(record(captured, id).paramsJson == record(graph, id).paramsJson);
  }
  plugin.inspectInitialValues = {};
  for (const auto &invalid : std::vector<std::pair<const char *, const char *>>{
           {"integer", "1.5"}, {"integer", "2147483648"}, {"boolean", "1"},
           {"choice", "2"}, {"choice", "\"Second\""}, {"text", "5"}, {"privateText", "false"},
           {"vector", "[1,2]"}, {"vector", "[1,2,1e999]"}, {"intVector", "[1.5,2]"}}) {
    const int creates = plugin.creates, destroys = plugin.destroys;
    auto bad = graph;
    for (auto &node : bad.nodes) if (node.id == id.value) node.paramsJson[invalid.first] = invalid.second;
    bool rejected = false;
    try { (void)document_detail::restoreGraph(bad); }
    catch (const std::runtime_error &) { rejected = true; }
    // Integral choice membership is deferred; JSON/type errors still abort before creation.
    const int created = std::strcmp(invalid.first, "choice") == 0 && std::strcmp(invalid.second, "2") == 0 ? 1 : 0;
    CHECK(rejected && plugin.creates == creates + created && plugin.destroys == destroys + created);
  }
  std::puts("ok  OFX initial typed values: integer, boolean, static choice, floating/integer vectors, string and hidden custom values use the real suite; strict refusal before publication");
  return true;
}

const OfxParameterSuiteV1 &params() {
  return *static_cast<const OfxParameterSuiteV1 *>(gOfxHost.fetchSuite(gOfxHost.host, kOfxParameterSuite, 1));
}
const OfxPropertySuiteV1 &props() {
  return *static_cast<const OfxPropertySuiteV1 *>(gOfxHost.fetchSuite(gOfxHost.host, kOfxPropertySuite, 1));
}

struct PrivateState {
  double hiddenGain = 1;
  bool deferred = false, safe = true;
  int syncs = 0;
  OfxStatus status = kOfxStatOK;
  RenderRuntime *runtime = nullptr;
  std::thread::id controlThread = std::this_thread::get_id();
};
PrivateState &privateState(Effect *effect) {
  void *data = nullptr;
  props().propGetPointer(H(&effect->props), kOfxPropInstanceData, 0, &data);
  return *static_cast<PrivateState *>(data);
}
Effect *effect(Node &node) { return static_cast<OfxProcessor *>(node.processor.get())->effect(); }
OfxPropertySetHandle paramSet(Effect *effect) {
  OfxPropertySetHandle properties = nullptr;
  params().paramSetGetPropertySet(reinterpret_cast<OfxParamSetHandle>(effect), &properties);
  return properties;
}
OfxParamHandle paramHandle(Effect *effect, const char *id) {
  OfxParamHandle param = nullptr;
  params().paramGetHandle(reinterpret_cast<OfxParamSetHandle>(effect), id, &param, nullptr);
  return param;
}

// Reuse the persistence descriptor, but keep a deferred private gain separate
// from its saved hidden parameter. All callback reads/writes use real OFX suites.
// Fresh instances read current parameters during Render: this deliberately does
// not depend on the separate, still-unfixed pre-CreateInstance restore ordering.
struct SyncPlugin : Plugin {
  SyncPlugin() {
    plugin.pluginIdentifier = "org.rawnode.selftest.private-sync";
    plugin.mainEntry = [](const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
      auto *instance = static_cast<Effect *>(const_cast<void *>(handle));
      if (!std::strcmp(action, kOfxActionCreateInstance))
        return props().propSetPointer(H(&instance->props), kOfxPropInstanceData, 0, new PrivateState);
      if (!std::strcmp(action, kOfxActionDestroyInstance)) {
        delete &privateState(instance);
        return kOfxStatOK;
      }
      if (!std::strcmp(action, kOfxActionSyncPrivateData)) {
        auto &state = privateState(instance);
        ++state.syncs;
        state.safe &= !in && !out && std::this_thread::get_id() == state.controlThread;
        if (state.runtime) {
          const auto ownership = RenderRuntimeTestAccess::snapshot(*state.runtime);
          state.safe &= ownership.mutationDepth > 0 && !ownership.previewBusy && !ownership.exportBusy;
        }
        if (state.status == kOfxStatReplyDefault) return state.status;
        // This setter takes gValueMutex. Calling the action under that lock
        // deadlocks. Even a failed action writes a partial live value first.
        if (state.deferred && params().paramSetValue(paramHandle(instance, "hiddenPersistent"), state.hiddenGain))
          return kOfxStatFailed;
        return state.status;
      }
      if (!std::strcmp(action, kOfxImageEffectActionRender)) {
        double visible = 0, hidden = 0;
        params().paramGetValue(paramHandle(instance, "visiblePersistent"), &visible);
        params().paramGetValue(paramHandle(instance, "hiddenPersistent"), &hidden);
        const auto &state = privateState(instance);
        const float gain = float(visible * (state.deferred ? state.hiddenGain : hidden));
        for (int i = 0; i < instance->w * instance->h * 4; ++i)
          instance->dst[i] = instance->src[i] * (i % 4 == 3 ? 1.0f : gain);
        return kOfxStatOK;
      }
      return kOfxStatReplyDefault;
    };
  }
};

void defer(App &app, Node &node, double hiddenGain) {
  auto &state = privateState(effect(node));
  state.hiddenGain = hiddenGain;
  state.deferred = true;
  state.runtime = &app.renderer;
}
std::string contents(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
bool syncValues(const PersistGraph &graph, const NodeId &id, const char *hidden) {
  CHECK(record(graph, id).paramsJson.at("visiblePersistent") == "2");
  CHECK(record(graph, id).paramsJson.at("hiddenPersistent") == hidden);
  return true;
}
bool syncRoundTrips(SyncPlugin &plugin) {
  Files files; Ui ui; App app;
  const auto photo = files.path("sync.png"), preset = files.path("sync-preset.rawnode.json");
  Image input; input.w = input.h = 1; input.px = {0.25f, 0.5f, 0.75f, 1};
  CHECK(writeImage(input, photo, {RgbGamut::Rec709, TransferFunction::Linear}) && openPath(app, photo));
  CHECK(addNode(app, plugin.index));
  const auto id = *app.selectedNodeId(); auto &node = *findNode(app, id);
  CHECK(setNodeParameterValue(app, id, "visiblePersistent", 2.0));
  CHECK(setNodeParameterValue(app, id, "hiddenPersistent", 3.0));
  defer(app, node, 5);
  auto &state = privateState(effect(node));
  // Without sync the parameters say gain 6, while the live private gain is 10.
  CHECK(parameter(node, "hiddenPersistent").value == ParameterValue(3.0) && pixels(app, 10));
  ui.inspector(app, "");
  CHECK(state.syncs == 0); // Inspector/ordinary reads must remain cheap.
  CHECK(saveCurrentInputSidecar(app));
  PersistFile saved; std::string error;
  CHECK(readGraphSidecar(inputSidecarPath(photo), saved, error) == SidecarReadStatus::Loaded && saved.grade);
  CHECK(syncValues(saved.grade->graph, id, "5"));
  CHECK(state.syncs == 1 && state.safe);
  App reopened;
  CHECK(openPath(reopened, photo) && parameter(*findNode(reopened, id), "hiddenPersistent").value == ParameterValue(5.0));
  CHECK(pixels(reopened, 10)); // (.25,-.5,2,.375) -> (2.5,-5,20,.375).

  defer(app, node, 7);
  CHECK(savePreset(app, preset) && state.syncs == 2 && state.safe);
  PersistGrade grade;
  CHECK(loadPresetFile(preset, grade, error) && syncValues(grade.graph, id, "7"));
  const auto destination = [&](App &target, const char *name) {
    const auto path = files.path(name);
    return writeImage(input, path, {RgbGamut::Rec709, TransferFunction::Linear}) && openPath(target, path);
  };
  App presetTarget;
  CHECK(destination(presetTarget, "preset.png") && loadPresetDialog(presetTarget, false, preset));
  CHECK(parameter(*findNode(presetTarget, id), "hiddenPersistent").value == ParameterValue(7.0) && pixels(presetTarget, 14));

  defer(app, node, 11);
  CHECK(copyGrade(app) && state.syncs == 3 && state.safe);
  PersistTransfer transfer;
  CHECK(decodeTransferPayload(ui.clipboard, transfer, error));
  CHECK(syncValues(std::get<PersistGrade>(transfer).graph, id, "11"));
  App gradeTarget;
  CHECK(destination(gradeTarget, "grade.png") && pasteFromClipboard(gradeTarget));
  CHECK(parameter(*findNode(gradeTarget, id), "hiddenPersistent").value == ParameterValue(11.0) && pixels(gradeTarget, 22));

  defer(app, node, 13);
  CHECK(copySelectedNode(app) && state.syncs == 4 && state.safe);
  CHECK(decodeTransferPayload(ui.clipboard, transfer, error));
  CHECK(std::get<PersistFragment>(transfer).nodes.front().paramsJson.at("hiddenPersistent") == "13");
  App nodeTarget;
  CHECK(destination(nodeTarget, "node.png") && pasteFromClipboard(nodeTarget));
  CHECK(parameter(*findNode(nodeTarget, nodeTarget.documentOutput.nodeId), "hiddenPersistent").value == ParameterValue(13.0));
  CHECK(pixels(nodeTarget, 26));

  // Automatic same-photo save-before-reopen destroys the original instance.
  defer(app, node, 17);
  CHECK(openPath(app, photo));
  CHECK(parameter(*findNode(app, id), "hiddenPersistent").value == ParameterValue(17.0) && pixels(app, 34));
  defer(app, *findNode(app, id), 19);
  CHECK(preparePhotoDeparture(app)); // Same boundary used by close/shutdown.
  App departed;
  CHECK(openPath(departed, photo));
  CHECK(parameter(*findNode(departed, id), "hiddenPersistent").value == ParameterValue(19.0) && pixels(departed, 38));
  std::puts("ok  OFX private sync: stale private/parameter state, sidecar reopen, automatic save, preset and grade/node clipboard with independent pixels");
  return true;
}

bool syncFlagsAndCaptures(SyncPlugin &plugin) {
  for (int flag : {-1, 0, 1}) {
    App app; CHECK(addNode(app, plugin.index));
    const auto id = *app.selectedNodeId(); auto &node = *findNode(app, id);
    auto *instance = effect(node); auto &state = privateState(instance);
    CHECK(setNodeParameterValue(app, id, "visiblePersistent", 2.0));
    CHECK(setNodeParameterValue(app, id, "hiddenPersistent", 3.0));
    defer(app, node, 5);
    // Decoys prove the flag is read from the parameter-set instance only.
    props().propSetInt(H(&instance->props), kOfxPropParamSetNeedsSyncing, 0, 0);
    props().propSetInt(H(&findParam(instance, "hiddenPersistent")->props), kOfxPropParamSetNeedsSyncing, 0, 0);
    if (flag >= 0) props().propSetInt(paramSet(instance), kOfxPropParamSetNeedsSyncing, 0, flag);
    PersistGrade grade;
    CHECK(captureGrade(app, grade));
    CHECK(syncValues(grade.graph, id, flag == 0 ? "3" : "5") && state.syncs == (flag == 0 ? 0 : 1) && state.safe);
    int actual = -1;
    const auto status = props().propGetInt(paramSet(instance), kOfxPropParamSetNeedsSyncing, 0, &actual);
    CHECK(flag < 0 ? status == kOfxStatErrUnknown : status == kOfxStatOK && actual == 0);
    defer(app, node, 7); // Do not reassert the flag yet.
    CHECK(captureGrade(app, grade) && state.syncs == (flag < 0 ? 2 : flag));
    CHECK(syncValues(grade.graph, id, flag < 0 ? "7" : flag == 0 ? "3" : "5"));
    if (flag >= 0) {
      props().propSetInt(paramSet(instance), kOfxPropParamSetNeedsSyncing, 0, 1);
      CHECK(captureGrade(app, grade) && syncValues(grade.graph, id, "7"));
      CHECK(props().propGetInt(paramSet(instance), kOfxPropParamSetNeedsSyncing, 0, &actual) == kOfxStatOK && actual == 0);
    }
    // ReplyDefault is the specified successful no-op for unsupported actions.
    state.status = kOfxStatReplyDefault;
    props().propSetInt(paramSet(instance), kOfxPropParamSetNeedsSyncing, 0, 1);
    const auto calls = state.syncs;
    CHECK(captureGrade(app, grade) && state.syncs == calls + 1);
    CHECK(props().propGetInt(paramSet(instance), kOfxPropParamSetNeedsSyncing, 0, &actual) == kOfxStatOK && actual == 0);
  }

  App app; CHECK(addNode(app, plugin.index));
  const auto first = *app.selectedNodeId();
  CHECK(addNode(app, plugin.index)); const auto second = *app.selectedNodeId();
  auto &a = *findNode(app, first); auto &b = *findNode(app, second);
  defer(app, a, 5); defer(app, b, 7);
  setNodeEnabled(app, second, false); // Disabled nodes still need capture.
  PersistFragment fragment; std::vector<std::string> warnings;
  CHECK(captureNodeFragment(app, {second, first}, fragment, warnings));
  CHECK(fragment.nodes[0].paramsJson.at("hiddenPersistent") == "7" && fragment.nodes[1].paramsJson.at("hiddenPersistent") == "5");
  auto &as = privateState(effect(a)); auto &bs = privateState(effect(b));
  CHECK(as.syncs == 1 && bs.syncs == 1 && as.safe && bs.safe);
  PersistNode single; defer(app, a, 11);
  CHECK(captureNode(app, first, single) && single.paramsJson.at("hiddenPersistent") == "11");
  CHECK(as.syncs == 2 && bs.syncs == 1);
  // Low-level detached graph/V2 captures borrow explicit execution exclusion.
  app.nodePositions.clear();
  document_detail::DocumentMutation ownership(app);
  defer(app, a, 13);
  const auto output = app.documentOutput;
  app.documentOutput = {app.primarySource, ports::image}; // Both processors are unreachable.
  CHECK(record(captureGraph({app.nodes, app.primarySource, app.documentOutput}), first).paramsJson.at("hiddenPersistent") == "13");
  CHECK(as.syncs == 3 && bs.syncs == 2);
  app.documentOutput = output;
  defer(app, a, 17);
  CHECK(captureChain(app).nodes[0].paramsJson.at("hiddenPersistent") == "17");
  CHECK(as.syncs == 4 && bs.syncs == 3 && as.safe && bs.safe);
  std::puts("ok  OFX private sync: absent/0/1 parameter-set flags, clearing, ReplyDefault, one action per captured node, fragments and low-level captures");
  return true;
}

bool failedSyncPreservesCaptures(SyncPlugin &plugin) {
  Files files; Ui ui; App app;
  const auto photo = files.path("failure.png"), preset = files.path("valid-preset.rawnode.json");
  const auto newPreset = files.path("failed-preset.rawnode.json");
  Image input; input.w = input.h = 1; input.px = {0.25f, 0.5f, 0.75f, 1};
  CHECK(writeImage(input, photo, {RgbGamut::Rec709, TransferFunction::Linear}) && openPath(app, photo));
  CHECK(addNode(app, plugin.index)); const auto id = *app.selectedNodeId(); auto &node = *findNode(app, id);
  CHECK(setNodeParameterValue(app, id, "visiblePersistent", 2.0));
  defer(app, node, 5);
  CHECK(saveCurrentInputSidecar(app) && savePreset(app, preset) && copyGrade(app));
  const auto sidecar = inputSidecarPath(photo), validSidecar = contents(sidecar), validPreset = contents(preset);
  const auto validClipboard = ui.clipboard;
  PersistGrade grade; CHECK(captureGrade(app, grade));
  std::string original, error; CHECK(encodeTransferPayload(grade, original, error));
  auto &state = privateState(effect(node));
  for (const auto status : {kOfxStatFailed, kOfxStatErrFatal}) {
    state.status = status; defer(app, node, 23);
    props().propSetInt(paramSet(effect(node)), kOfxPropParamSetNeedsSyncing, 0, 1);
    CHECK(!saveCurrentInputSidecar(app, &error));
    CHECK(error.find("SyncPrivateData") != std::string::npos && error.find(plugin.plugin.pluginIdentifier) != std::string::npos);
    CHECK(contents(sidecar) == validSidecar && !app.persistenceWarning.empty());
    CHECK(parameter(node, "hiddenPersistent").value == ParameterValue(23.0)); // Callback did run a setter before failure.
    int flag = 0;
    CHECK(props().propGetInt(paramSet(effect(node)), kOfxPropParamSetNeedsSyncing, 0, &flag) == kOfxStatOK && flag == 1);
    bool confirmed = false;
    CHECK(!savePreset(app, preset, [&](const auto &) { confirmed = true; return true; }) && !confirmed);
    CHECK(contents(preset) == validPreset && !savePreset(app, newPreset) && !fs::exists(newPreset));
    CHECK(!copyGrade(app) && ui.clipboard == validClipboard);
    CHECK(!copySelectedNode(app) && ui.clipboard == validClipboard);
    CHECK(!captureGrade(app, grade));
    std::string after; CHECK(encodeTransferPayload(grade, after, error) && after == original);
    PersistFragment fragment; fragment.positions["untouched"] = {1, 2};
    std::vector<std::string> warnings{"untouched"};
    CHECK(!captureNodeFragment(app, {id}, fragment, warnings));
    CHECK(fragment.nodes.empty() && fragment.positions.count("untouched") && warnings == std::vector<std::string>{"untouched"});
    CHECK(!openPath(app, photo) && findNode(app, id) == &node && contents(sidecar) == validSidecar);
    CHECK(state.safe && !preparePhotoDeparture(app) && contents(sidecar) == validSidecar);
  }
  state.status = kOfxStatOK;
  CHECK(saveCurrentInputSidecar(app) && app.persistenceWarning.empty());
  App reopened; CHECK(openPath(reopened, photo) && pixels(reopened, 46));
  std::puts("ok  OFX private sync failures: partial callback writes cannot publish sidecar/preset/clipboard or replace capture outputs; retry succeeds");
  return true;
}
}  // namespace

bool testOfxPersistence() {
  Plugin plugin;
  const bool passed = dynamicChoiceRestoration() && cachedGainFiveReopen(plugin) && productionRoundTrips(plugin) &&
                      nonpersistentRestoreGuards(plugin) && initialValueValidationAndLifecycle(plugin) &&
                      typedInitialValues(plugin);
  CHECK(plugin.live == 0 && plugin.invalidDestroys == 0 && plugin.preCreationNotifications == 0);
  if (!passed) return false;
  SyncPlugin sync;
  return syncRoundTrips(sync) && syncFlagsAndCaptures(sync) && failedSyncPreservesCaptures(sync);
}
