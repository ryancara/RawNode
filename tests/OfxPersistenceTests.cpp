#include "OfxPersistenceTests.h"

#include "GraphEvaluator.h"
#include "NodeGraph.h"
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
  int creates = 0, destroys = 0, live = 0, invalidDestroys = 0;
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
        if (!cache) return kOfxStatFailed;
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
  plugin.createStatus = kOfxStatOK;
  std::puts("ok  OFX construction: missing defaults, no second restore, silent preparation, validation aborts, failed creation, missing processor retention and balanced destruction");
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
  const int creates = plugin.creates, destroys = plugin.destroys;
  for (const auto &invalid : std::vector<std::pair<const char *, const char *>>{
           {"integer", "1.5"}, {"integer", "2147483648"}, {"boolean", "1"},
           {"choice", "2"}, {"choice", "\"Second\""}, {"text", "5"}, {"privateText", "false"},
           {"vector", "[1,2]"}, {"vector", "[1,2,1e999]"}, {"intVector", "[1.5,2]"}}) {
    auto bad = graph;
    for (auto &node : bad.nodes) if (node.id == id.value) node.paramsJson[invalid.first] = invalid.second;
    bool rejected = false;
    try { (void)document_detail::restoreGraph(bad); }
    catch (const std::runtime_error &) { rejected = true; }
    CHECK(rejected && plugin.creates == creates && plugin.destroys == destroys);
  }
  std::puts("ok  OFX initial typed values: integer, boolean, choice, floating/integer vectors, string and hidden custom values use the real suite; strict invalid-value refusal precedes creation");
  return true;
}
}  // namespace

bool testOfxPersistence() {
  Plugin plugin;
  const bool passed = cachedGainFiveReopen(plugin) && productionRoundTrips(plugin) &&
                      nonpersistentRestoreGuards(plugin) && initialValueValidationAndLifecycle(plugin) && typedInitialValues(plugin);
  CHECK(plugin.live == 0 && plugin.invalidDestroys == 0);
  return passed;
}
