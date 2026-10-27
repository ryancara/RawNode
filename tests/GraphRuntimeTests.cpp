#include "GraphRuntimeTests.h"

#include "AppState.h"
#include "DocumentMutation.h"
#include "Export.h"
#include "GraphCommands.h"
#include "GraphDepthFixtures.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActions.h"
#include "ofx/OfxHost.h"
#include "ofxParam.h"
#include "ui/UiContext.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "graph runtime check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
static OutputRef image(const NodeId &id) { return {id, ports::image}; }
static OutputRef mask(const NodeId &id) { return {id, ports::mask}; }
static InputAddress input(const NodeId &id, const PortId &port = ports::image) { return {id, std::nullopt, port}; }
static bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto state) {
    return !state.previewPending && !state.previewBusy && !state.displayPending;
  });
}
static Image raster(int w, int h, float red = 0.25f) {
  Image value; value.w = w; value.h = h;
  for (int i = 0; i < w * h; ++i) value.px.insert(value.px.end(), {red, red, red, 0.75f});
  return value;
}
static void source(App &app) {
  app.full = raster(2, 1);
  app.preview = std::make_shared<Image>(raster(1, 1));
}
static bool add(App &app, const NodeId &id, NodeOperation op) {
  Node node(op); node.id = id;
  return graph::addNode(app, std::move(node)).ok();
}
static App::PreviewPublication publication(App &app) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return app.processed;
}
static bool diagnostic(const EvaluationResult &result, const std::string &text) {
  for (const auto &d : result.diagnostics) if (d.message.find(text) != std::string::npos) return true;
  return false;
}
struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
      ("rawnode-6d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { std::filesystem::create_directories(dir); }
  ~Files() { std::error_code error; std::filesystem::remove_all(dir, error); }
  std::string path(const char *name) { return (dir / name).string(); }
};

// Test-owned barriers always have an independent release path. A failed
// ownership/lock regression times out and fails instead of hanging selftest.
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool released = true;
  int entered = 0;
  void arm() { std::lock_guard<std::mutex> lock(mutex); released = false; }
  void hold() {
    std::unique_lock<std::mutex> lock(mutex);
    if (released) return;
    ++entered; cv.notify_all(); cv.wait(lock, [&] { return released; });
  }
  bool wait(int count) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered >= count; });
  }
  void unblock() { { std::lock_guard<std::mutex> lock(mutex); released = true; } cv.notify_all(); }
};
struct GateWorkerGuard {
  App &app; Gate &gate;
  ~GateWorkerGuard() {
    gate.unblock(); app.renderer.shutdown();
    RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  }
};

class NumericProcessor final : public Processor {
 public:
  float gain = 1.0f, offset = 0.0f;
  std::atomic<int> calls{0}, width{0}, height{0};
  std::atomic<bool> fail{false};
  Gate *renderGate = nullptr;
  RenderRuntime *captureRuntime = nullptr;
  std::atomic<bool> rendering{false};
  bool failCapture = false;
  mutable int captures = 0;
  mutable bool unsafeCapture = false;
  int syncs = 0;
  bool unsafeSync = false;
  const float *lastInput = nullptr;
  ProcessorBackend backend() const override { return ProcessorBackend::Native; }
  std::string identifier() const override { return "test.numeric"; }
  std::string displayName() const override { return "Test numeric"; }
  ProcessorResult syncPrivateData() override {
    if (!captureRuntime) return ProcessorResult::success();
    ++syncs;
    const auto state = RenderRuntimeTestAccess::snapshot(*captureRuntime);
    unsafeSync |= rendering || state.previewBusy || state.exportBusy || state.mutationDepth == 0;
    return unsafeSync ? ProcessorResult::failure(92, "unsafe private sync") : ProcessorResult::success();
  }
  std::vector<ProcessorParameter> parameters() const override {
    if (!captureRuntime) return {};
    ++captures;
    const auto state = RenderRuntimeTestAccess::snapshot(*captureRuntime);
    unsafeCapture |= rendering || state.previewBusy || state.exportBusy;
    // A missing guard fails deterministically without introducing a test race.
    if (unsafeCapture || failCapture) throw std::runtime_error("capture fault");
    ProcessorParameter parameter;
    parameter.id = "gain"; parameter.type = ParameterType::Double; parameter.value = double(gain);
    return {parameter};
  }
  bool setParameterValue(const std::string &, const ParameterValue &, bool) override { return false; }
  bool resetParameter(const std::string &, bool) override { return false; }
  bool activateParameter(const std::string &) override { return false; }
  void setRenderSize(int w, int h) override { width = w; height = h; }
  ProcessorResult render(const Image &in, Image &out, const RenderCancellation &) override {
    rendering = true;
    struct RenderGuard { std::atomic<bool> &active; ~RenderGuard() { active = false; } } guard{rendering};
    ++calls; lastInput = in.px.data();
    if (renderGate) renderGate->hold();
    if (fail) return ProcessorResult::failure(91, "runtime backend failure");
    out = in;
    for (size_t i = 0; i < out.px.size(); ++i) if (i % 4 != 3) out.px[i] = out.px[i] * gain + offset;
    return ProcessorResult::success();
  }
};
static NumericProcessor *numeric(App &app, const NodeId &id, float gain = 1.0f, float offset = 0.0f) {
  auto processor = std::make_unique<NumericProcessor>(); auto *p = processor.get();
  p->gain = gain; p->offset = offset;
  Node node; node.id = id; node.processor = std::move(processor);
  const auto result = graph::insertImageNode(app, std::move(node));
  return result.ok() ? p : nullptr;
}

// Pixel operations, blocking barriers and counters are test-owned. The runtime
// still invokes the actual GraphEvaluator and owns every execution/publication.
class Executor final : public graph_test::Executor {
 public:
  std::mutex mutex;
  std::condition_variable cv;
  std::string blocked;
  bool release = true, exportMismatch = false, cancelled = false, blockAtPublication = false;
  EvaluationState state = EvaluationState::Available;
  int entered = 0, calls = 0;
  void block(const NodeId &id, bool publishing = false) {
    std::lock_guard<std::mutex> lock(mutex); blocked = id.value; release = false; blockAtPublication = publishing;
  }
  void unblock() { { std::lock_guard<std::mutex> lock(mutex); release = true; } cv.notify_all(); }
  bool wait(int count) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered >= count; });
  }
  void beforePublication(const OutputRef &endpoint) {
    std::unique_lock<std::mutex> lock(mutex);
    if (blockAtPublication && endpoint.nodeId.value == blocked) {
      ++entered; cv.notify_all(); cv.wait(lock, [&] { return release; });
    }
  }
  graph_test::OperationResult execute(const Node &node, const EvaluationContext &context,
                                     const graph_test::InputValues &in, const RenderCancellation &) override {
    std::unique_lock<std::mutex> lock(mutex);
    ++calls;
    if (!blockAtPublication && node.id.value == blocked) { ++entered; cv.notify_all(); cv.wait(lock, [&] { return release; }); }
    if (cancelled) return {EvaluationState::Cancelled, {}, {}};
    if (state != EvaluationState::Available) return {state, {}, "runtime inspection failure"};
    if (node.operation == NodeOperation::TestUnavailableMask || node.operation == NodeOperation::TestUnavailableImage)
      return {EvaluationState::Unavailable, {}, "runtime test data unavailable"};
    if (node.operation == NodeOperation::TestImage || node.operation == NodeOperation::TestMaskedImage)
      return {EvaluationState::Available, {{ports::image, ImageValue(std::make_shared<Image>(*in.image))}}, {}};
    auto value = std::make_shared<Mask>();
    const auto frame = in.reference ? in.reference : in.image;
    value->w = frame->w; value->h = frame->h;
    if (exportMismatch && context.purpose == EvaluationPurpose::Export) ++value->w;
    value->px.assign(static_cast<size_t>(value->w) * value->h, 0.25f);
    return {EvaluationState::Available, {{ports::mask, MaskValue(value)}}, {}};
  }
};
struct WorkerGuard {
  App &app; Executor &executor;
  ~WorkerGuard() { executor.unblock(); app.renderer.shutdown(); }
};

static bool testStrengthEditOwnership() {
  App app; source(app); Gate gate;
  const NodeId effect{"strength-exclusion"};
  auto *processor = numeric(app, effect, 2); CHECK(processor);
  gate.arm(); processor->renderGate = &gate;
  struct Probe { Gate &gate; bool waited = false; } probe{gate};
  GateWorkerGuard guard{app, gate};
  app.renderer.start(); app.renderer.requestPreview(); CHECK(gate.wait(1));
  RenderRuntimeTestAccess::observe(app.renderer, &probe, [](void *context, auto event) noexcept {
    if (event == RenderRuntimeTestAccess::Event::MutationWaiting) {
      auto &probe = *static_cast<Probe *>(context);
      probe.waited = true;
      probe.gate.unblock(); // Independent release if the regression returns.
    }
  });
  for (int i = 0; i < 100; ++i) CHECK(setNodeStrength(app, effect, (i % 2) ? .75 : .25));
  CHECK(setNodeStrength(app, effect, 0));
  CHECK(!probe.waited && processor->rendering && processor->calls == 1);
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).previewPending);
  gate.unblock();
  CHECK(idle(app));
  CHECK(processor->calls == 1 && findNode(app, effect)->strength == 0);
  const auto frame = publication(app);
  CHECK(frame.result.state == EvaluationState::Available && std::get<ImageValue>(frame.result.value) == app.preview);
  // Exercise actual planner reads concurrently with a stream of live edits.
  for (int i = 0; i < 1000; ++i) CHECK(setNodeStrength(app, effect, (i % 2) ? .75 : .25));
  CHECK(setNodeStrength(app, effect, .5) && idle(app));
  const auto latest = publication(app);
  CHECK(latest.result.state == EvaluationState::Available && processor->calls > 1 && !probe.waited);
  CHECK(std::get<ImageValue>(latest.result.value)->px == std::vector<float>({.375f, .375f, .375f, .75f}));
  {
    auto exportLease = app.renderer.acquireExport(); CHECK(exportLease);
    CHECK(!setNodeStrength(app, effect, .75) && findNode(app, effect)->strength == .5);
  }
  CHECK(idle(app));
  return true;
}

static bool testTopologyAndV2() {
  App app; source(app);
  CHECK(addNativeExposureNode(app) && addNativeCstNode(app));
  const auto first = app.nodes[1].id, second = app.nodes[2].id;
  CHECK(app.nodes[1].fixedInputs.at(ports::image) == image(app.primarySource));
  CHECK(app.nodes[2].fixedInputs.at(ports::image) == image(first) && app.documentOutput == image(second));
  app.nodePositions.clear(); // Historical V2 fixtures intentionally omit layout.
  auto saved = captureChain(app);
  moveNode(app, app.nodes[2].id, app.nodes[1].id);
  CHECK(app.nodes[1].id == first && app.nodes[2].id == second && app.documentOutput == image(first));
  CHECK(app.nodes[1].fixedInputs.at(ports::image) == image(second) && app.selectedNodeId() == app.nodes[2].id);
  CHECK(captureChain(app).nodes[0].id == second.value);
  App restored; source(restored); applyChain(restored, captureChain(app));
  CHECK(restored.nodes[1].id == second && restored.nodes[2].id == first && restored.documentOutput == image(first));
  CHECK(restored.nodes[1].fixedInputs.at(ports::image) == image(restored.primarySource) &&
        restored.nodes[2].fixedInputs.at(ports::image) == image(second));
  destroyNode(app, app.nodes[2].id);
  CHECK(app.documentOutput == image(first) && app.nodes[1].fixedInputs.at(ports::image) == image(app.primarySource));
  PersistNode missing; missing.id = "preserved-missing"; missing.backend = "dctl";
  missing.identifier = "asset.dctl"; missing.paramsJson["opaque"] = "[1,2]";
  saved.nodes.push_back(missing); saved.selectedNodeId = missing.id;
  applyChain(restored, saved);
  CHECK(restored.selectedNodeId() == restored.nodes[3].id && restored.documentOutput == image(NodeId{missing.id}));
  CHECK(restored.nodes[3].fixedInputs.at(ports::image) == image(second));
  CHECK(captureChain(restored).nodes.back().paramsJson == missing.paramsJson);

  // Noncommutative processor maths proves topology controls execution, rather
  // than merely checking the row/capture order.
  App ordered; source(ordered);
  const NodeId a{"gain"}, b{"offset"};
  CHECK(numeric(ordered, a, 2.0f) && numeric(ordered, b, 1.0f, 1.0f));
  const EvaluationContext context{EvaluationPurpose::Inspection, 2, 1, 1, 1};
  auto result = ordered.renderer.evaluateOwned(ordered.documentOutput, context, ordered.preview);
  CHECK(std::get<ImageValue>(result.value)->px[0] == 1.5f);
  const auto selected = ordered.selectedNodeId();
  moveNode(ordered, ordered.nodes[2].id, ordered.nodes[1].id);
  result = ordered.renderer.evaluateOwned(ordered.documentOutput, context, ordered.preview);
  CHECK(std::get<ImageValue>(result.value)->px[0] == 2.5f && ordered.nodes[1].id == a && ordered.nodes[2].id == b);
  CHECK(ordered.selectedNodeId() == selected);
  destroyNode(ordered, ordered.nodes[2].id);
  result = ordered.renderer.evaluateOwned(ordered.documentOutput, context, ordered.preview);
  CHECK(std::get<ImageValue>(result.value)->px[0] == 0.5f);
  return true;
}

static bool testTypedPublicationAndDisplay() {
  Executor executor; App app; source(app);
  const NodeId effect{"effect"}, maskNode{"mask"}, unavailable{"missing-mask"};
  auto *processor = numeric(app, effect, 2.0f); CHECK(processor);
  CHECK(add(app, maskNode, NodeOperation::TestImageToMask) && add(app, unavailable, NodeOperation::TestUnavailableMask));
  CHECK(graph::connect(app, image(app.primarySource), input(maskNode)).ok());
  RenderRuntimeTestAccess::evaluation(app.renderer, &executor);
  WorkerGuard guard{app, executor}; app.renderer.start(); app.renderer.requestDocumentPreview();
  CHECK(idle(app));
  auto frame = publication(app);
  CHECK(frame.result.requested == app.documentOutput && std::get<ImageValue>(frame.result.value)->px[0] == 0.5f);
  const auto initialCalls = processor->calls.load();
  { std::lock_guard<std::mutex> lock(app.colorMutex); app.outputEncoding.gamma = TransferFunction::Linear; }
  app.renderer.requestDisplayRefresh(); CHECK(idle(app));
  CHECK(processor->calls == initialCalls && publication(app).generation == frame.generation);
  app.renderer.requestInspection(image(app.primarySource)); CHECK(idle(app));
  auto sourceFrame = publication(app);
  CHECK(std::get<ImageValue>(sourceFrame.result.value).get() == app.preview.get());
  std::vector<unsigned char> expected;
  toDisplayRGBA8(*app.preview, app.inputEncoding, expected);
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA == expected); }
  auto oldSource = std::get<ImageValue>(sourceFrame.result.value);
  { document_detail::DocumentMutation mutation(app); app.preview = std::make_shared<Image>(raster(1, 1, 0.4f)); mutation.changed(); }
  CHECK(idle(app) && oldSource->px[0] == 0.25f && std::get<ImageValue>(publication(app).result.value)->px[0] == 0.4f);
  app.renderer.requestInspection(mask(maskNode)); CHECK(idle(app));
  frame = publication(app);
  CHECK(frame.result.state == EvaluationState::Available && frame.result.valueType == ValueType::Mask);
  CHECK(frame.request && *frame.request == mask(maskNode) && app.documentOutput == image(effect));
  std::vector<unsigned char> maskDisplay;
  { std::lock_guard<std::mutex> lock(app.displayMutex); maskDisplay = app.displayRGBA; }
  CHECK(maskDisplay == std::vector<unsigned char>({64,64,64,255}));
  { std::lock_guard<std::mutex> lock(app.colorMutex); app.outputEncoding = {RgbGamut::Rec2020, TransferFunction::SRGB}; }
  int calls;
  { std::lock_guard<std::mutex> lock(executor.mutex); calls = executor.calls; }
  app.renderer.requestDisplayRefresh(); CHECK(idle(app));
  { std::lock_guard<std::mutex> lock(executor.mutex); CHECK(executor.calls == calls); }
  CHECK(std::get<MaskValue>(publication(app).result.value) == std::get<MaskValue>(frame.result.value));
  CHECK(std::get<MaskValue>(frame.result.value)->px[0] == 0.25f);
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA == maskDisplay); }
  app.renderer.requestInspection(mask(unavailable)); CHECK(idle(app));
  frame = publication(app);
  CHECK(frame.result.state == EvaluationState::Unavailable && frame.result.valueType == ValueType::Mask);
  CHECK(std::holds_alternative<std::monostate>(frame.result.value) && diagnostic(frame.result, "unavailable"));
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA.empty() && app.display.w == 0); }
  processor->fail = true;
  app.renderer.requestDocumentPreview(); CHECK(idle(app));
  frame = publication(app);
  CHECK(frame.result.state == EvaluationState::Error && diagnostic(frame.result, "runtime backend failure"));
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA.empty()); }
  { std::lock_guard<std::mutex> lock(executor.mutex); executor.cancelled = true; }
  app.renderer.requestInspection(mask(maskNode)); CHECK(idle(app));
  CHECK(publication(app).generation == frame.generation); // Cancelled never publishes.
  return true;
}

static bool testInactiveInspectionAndRestoration() {
  Files files; Executor executor; App app; source(app);
  const NodeId effect{"inactive-export"}, generator{"inactive-inspection"};
  CHECK(numeric(app, effect) && add(app, generator, NodeOperation::TestReferenceMask));
  RenderRuntimeTestAccess::evaluation(app.renderer, &executor);
  WorkerGuard guard{app, executor}; app.renderer.start();
  app.renderer.requestInspection(mask(generator)); CHECK(idle(app));
  CHECK(publication(app).result.state == EvaluationState::Available);
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(!app.displayRGBA.empty()); }
  setNodeEnabled(app, generator, false); CHECK(idle(app));
  auto frame = publication(app);
  CHECK(frame.result.state == EvaluationState::Inactive && frame.result.valueType == ValueType::Mask &&
        frame.result.requested == mask(generator) && frame.result.diagnostics.empty() &&
        std::holds_alternative<std::monostate>(frame.result.value));
  CHECK(app.getStatus() == "Mask inactive (bypassed)");
  { std::lock_guard<std::mutex> lock(app.displayMutex);
    CHECK(app.displayRGBA.empty() && app.display.w == 0 && app.display.h == 0);
  }
  app.renderer.requestDisplayRefresh(); CHECK(idle(app));
  CHECK(publication(app).generation == frame.generation && app.getStatus() == "Mask inactive (bypassed)");
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA.empty()); }

  for (bool success : {true, false}) {
    const auto before = publication(app).generation;
    CHECK(runExportJob(app, files.path(success ? "inactive.png" : "absent/failed.png")) == success);
    CHECK(idle(app));
    frame = publication(app);
    CHECK(frame.result.state == EvaluationState::Inactive && frame.result.requested == mask(generator) &&
          frame.generation > before && frame.result.diagnostics.empty());
    CHECK(app.getStatus().find(success ? "Exported inactive.png" : "Export failed") == 0);
    { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA.empty() && app.display.w == 0); }
  }
  app.renderer.requestInspection(mask(generator)); CHECK(idle(app));
  CHECK(app.getStatus() == "Mask inactive (bypassed)");
  setNodeEnabled(app, generator, true); CHECK(idle(app));
  CHECK(publication(app).result.state == EvaluationState::Available);
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(!app.displayRGBA.empty()); }
  return true;
}

static bool testEndpointSupersession() {
  Executor executor; App app; source(app);
  const NodeId a{"image-a"}, b{"mask-b"};
  CHECK(add(app, a, NodeOperation::TestImage) && add(app, b, NodeOperation::TestImageToMask));
  CHECK(graph::connect(app, image(app.primarySource), input(a)).ok() && graph::connect(app, image(app.primarySource), input(b)).ok());
  CHECK(graph::setDocumentOutput(app, image(a)).ok());
  struct PublicationProbe {
    App &app;
    std::atomic<int> expected{-1};
    std::atomic<bool> stale{false};
  } probe{app};
  RenderRuntimeTestAccess::evaluation(app.renderer, &executor, [](void *context) {
    auto &probe = *static_cast<PublicationProbe *>(context);
    std::lock_guard<std::mutex> lock(probe.app.displayMutex);
    if (probe.expected >= 0 && probe.app.processed.generation != probe.expected) probe.stale = true;
  }, &probe);
  RenderRuntimeTestAccess::publication(app.renderer, [](void *context, const OutputRef &endpoint) {
    static_cast<Executor *>(context)->beforePublication(endpoint);
  }, &executor);
  WorkerGuard guard{app, executor}; executor.block(a); app.renderer.start(); app.renderer.requestDocumentPreview();
  CHECK(executor.wait(1));
  const auto before = publication(app).generation;
  app.renderer.requestInspection(mask(b));
  CHECK(publication(app).generation == before);
  executor.unblock(); CHECK(idle(app));
  auto frame = publication(app);
  CHECK(frame.result.requested == mask(b) && frame.result.valueType == ValueType::Mask && frame.result.state == EvaluationState::Available);
  executor.block(b); app.renderer.requestInspection(mask(b)); CHECK(executor.wait(2));
  app.renderer.requestDocumentPreview();
  CHECK(publication(app).generation == frame.generation);
  executor.unblock(); CHECK(idle(app)); frame = publication(app);
  CHECK(frame.result.requested == image(a) && frame.result.valueType == ValueType::Image && frame.result.state == EvaluationState::Available);
  CHECK(frame.generation == RenderRuntimeTestAccess::snapshot(app.renderer).epoch);
  // Both operations have returned and passed cooperative cancellation before
  // this barrier. The publication gate itself must reject the old generation.
  probe.expected = frame.generation;
  executor.block(a, true); app.renderer.requestDocumentPreview(); CHECK(executor.wait(3));
  app.renderer.requestInspection(mask(b)); executor.unblock(); CHECK(idle(app));
  CHECK(!probe.stale && publication(app).result.requested == mask(b));
  frame = publication(app); probe.expected = frame.generation;
  executor.block(b, true); app.renderer.requestInspection(mask(b)); CHECK(executor.wait(4));
  app.renderer.requestDocumentPreview(); executor.unblock(); CHECK(idle(app));
  CHECK(!probe.stale && publication(app).result.requested == image(a));
  probe.expected = -1;
  return true;
}

static bool testExportPolicyAndContext() {
  Files files; Executor executor; App app; source(app);
  const NodeId effect{"export-effect"}, unreachable{"off-spine-missing"}, missing{"reachable-missing"}, unavailable{"unavailable-image"};
  auto *processor = numeric(app, effect, 2.0f); CHECK(processor);
  CHECK(add(app, unreachable, NodeOperation::ImageProcessor));
  findNode(app, unreachable)->storedIdentifier = unreachable.value;
  RenderRuntimeTestAccess::evaluation(app.renderer, &executor);
  CHECK(runExportJob(app, files.path("available.png")) && processor->width == 2 && processor->height == 1);
  CHECK(processor->lastInput == app.full.px.data() && app.getStatus().find(unreachable.value) == std::string::npos);
  Image written; ColorEncoding encoding; bool raw;
  CHECK(loadImage(files.path("available.png"), written, encoding, raw) && written.w == 2 && written.h == 1);
  Node placeholder; placeholder.id = missing; placeholder.storedIdentifier = missing.value;
  CHECK(graph::insertImageNode(app, std::move(placeholder)).ok());
  CHECK(runExportJob(app, files.path("warnings.png")) && app.getStatus().find(missing.value) != std::string::npos);
  processor->fail = true;
  CHECK(!runExportJob(app, files.path("error.png")) && !std::filesystem::exists(files.path("error.png")));
  CHECK(app.getStatus().find("runtime backend failure") != std::string::npos);
  CHECK(add(app, unavailable, NodeOperation::TestUnavailableImage) && graph::setDocumentOutput(app, image(unavailable)).ok());
  CHECK(!runExportJob(app, files.path("unavailable.png")) && app.getStatus().find("unavailable") != std::string::npos);
  CHECK(!std::filesystem::exists(files.path("unavailable.png")));
  CHECK(graph::setDocumentOutput(app, image(app.primarySource)).ok());
  CHECK(!runExportJob(app, files.path("source-only.png"))); // Stored off-spine nodes do not enable export.
  app.renderer.start(); app.renderer.requestDocumentPreview(); CHECK(idle(app));
  CHECK(publication(app).result.requested == image(app.primarySource) && publication(app).result.state == EvaluationState::Available);
  app.renderer.shutdown();

  Executor sized; App dimensions; source(dimensions);
  const NodeId coverage{"resolution-mask"}, consumer{"coverage-consumer"};
  CHECK(add(dimensions, coverage, NodeOperation::TestReferenceMask) && add(dimensions, consumer, NodeOperation::TestMaskedImage));
  CHECK(graph::connect(dimensions, image(dimensions.primarySource), input(consumer)).ok());
  CHECK(graph::connect(dimensions, mask(coverage), input(consumer, ports::mask)).ok());
  CHECK(graph::setDocumentOutput(dimensions, image(consumer)).ok());
  sized.exportMismatch = true;
  RenderRuntimeTestAccess::evaluation(dimensions.renderer, &sized);
  WorkerGuard guard{dimensions, sized}; dimensions.renderer.start(); dimensions.renderer.requestDocumentPreview();
  CHECK(idle(dimensions) && publication(dimensions).result.state == EvaluationState::Available);
  CHECK(!runExportJob(dimensions, files.path("dimension-error.png")));
  CHECK(dimensions.getStatus().find("Image 2x1, Mask 3x1") != std::string::npos);
  CHECK(!std::filesystem::exists(files.path("dimension-error.png")) && idle(dimensions));
  return true;
}

static bool testWorkerExceptionBoundary() {
  Executor executor; App app; source(app);
  const NodeId endpoint{"surviving-worker"}; auto *processor = numeric(app, endpoint); CHECK(processor);
  std::atomic<int> attempts{0};
  RenderRuntimeTestAccess::evaluation(app.renderer, nullptr, [](void *context) {
    if (++*static_cast<std::atomic<int> *>(context) == 1) throw std::bad_alloc();
  }, &attempts);
  WorkerGuard guard{app, executor};
  // The topology edit already queued a preview. Replace it before starting the
  // worker so the injected exception cannot belong to a superseded request.
  app.renderer.requestDocumentPreview(); app.renderer.start(); CHECK(idle(app));
  auto result = publication(app);
  CHECK(result.result.state == EvaluationState::Error && result.result.requested == image(endpoint) && processor->calls == 0);
  CHECK(result.request && *result.request == image(endpoint) && diagnostic(result.result, "bad_alloc"));
  { document_detail::DocumentMutation mutation(app); mutation.changed(); }
  CHECK(idle(app) && publication(app).result.state == EvaluationState::Available && processor->calls == 1 && attempts == 2);
  return true;
}

static bool testPhotoSwitch(bool exporting) {
  Files files; Gate gate; App app;
  const auto photoA = files.path("photo-a.png"), photoB = files.path("photo-b.png");
  const auto output = files.path("export-a.png"), expected = files.path("expected-a.png");
  const ColorEncoding encoding{RgbGamut::Rec709, TransferFunction::Linear};
  CHECK(writeImage(raster(7, 3, 0.2f), photoA, encoding, 92));
  CHECK(writeImage(raster(4, 5, 0.8f), photoB, encoding, 92));
  openPath(app, photoA);
  const auto *pixelsA = app.full.px.data();
  auto *processor = numeric(app, NodeId{"switch-barrier"}); CHECK(processor);
  CHECK(writeImage(app.full, expected, app.outputEncoding, 92));
  processor->renderGate = &gate; gate.arm();
  struct MutationProbe {
    App &app;
    const float *pixelsA;
    std::mutex mutex;
    std::condition_variable cv;
    bool observed = false, intact = false;
  } probe{app, pixelsA};
  RenderRuntimeTestAccess::observe(app.renderer, &probe, [](void *context, auto event) noexcept {
    if (event != RenderRuntimeTestAccess::Event::MutationWaiting) return;
    auto &probe = *static_cast<MutationProbe *>(context);
    std::lock_guard<std::mutex> lock(probe.mutex);
    // Runs on the control thread before mutation starts changing the document.
    probe.intact = probe.app.full.px.data() == probe.pixelsA && probe.app.full.w == 7 && probe.app.full.h == 3;
    probe.observed = true; probe.cv.notify_all();
  });
  GateWorkerGuard guard{app, gate};
  if (exporting) CHECK(startExport(app, output));
  app.renderer.start();
  CHECK(gate.wait(1));
  bool waited = false;
  std::thread release([&] {
    std::unique_lock<std::mutex> lock(probe.mutex);
    waited = probe.cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe.observed; }) && probe.intact;
    lock.unlock();
    gate.unblock(); // Independent of the control thread blocked in openPath.
  });
  openPath(app, photoB);
  release.join();
  app.renderer.joinExport();
  CHECK(waited && idle(app));
  Image decodedB; ColorEncoding detected; bool raw;
  CHECK(loadImage(photoB, decodedB, detected, raw));
  CHECK(app.path == photoB && app.full.w == 4 && app.full.h == 5 && app.full.px == decodedB.px);
  CHECK(app.nodes.size() == 1 && app.documentOutput == image(app.primarySource)); // Nested sidecar clear.
  const auto frame = publication(app);
  CHECK(frame.result.state == EvaluationState::Available && frame.result.requested == image(app.primarySource));
  CHECK(std::get<ImageValue>(frame.result.value)->px == decodedB.px && app.getStatus() == "4×5 preview");
  if (exporting) {
    Image exported, reference;
    CHECK(loadImage(output, exported, detected, raw) && loadImage(expected, reference, detected, raw));
    CHECK(exported.w == 7 && exported.h == 3 && exported.px == reference.px);
  }
  return true;
}
static bool testExportPhotoSwitch() { return testPhotoSwitch(true); }
static bool testPreviewPhotoSwitch() { return testPhotoSwitch(false); }

// Both transfer operations must drain borrowers before swapping nodes or
// reallocating storage. The old export writes the accepted old grade; a preview
// after the edit evaluates the complete new grade through the shared runtime.
static bool testTransferLifetime(bool append, bool exporting) {
  Files files; Gate gate; App app; source(app);
  const NodeId oldId{"transfer-borrowed"};
  auto *processor = numeric(app, oldId, 2.0f); CHECK(processor);
  App incoming; CHECK(addNativeExposureNode(incoming));
  CHECK(selectedNode(incoming)->processor->setParameterValue("exposure", 1.0));
  PersistGrade grade; CHECK(captureGrade(incoming, grade));
  const auto oldFull = app.full.px.data(); const auto oldPreview = app.preview;
  const auto output = files.path("transfer-old.png"), expected = files.path("transfer-expected.png");
  auto reference = app.full;
  for (size_t i = 0; i < reference.px.size(); ++i) if (i % 4 != 3) reference.px[i] *= 2.0f;
  CHECK(writeImage(reference, expected, app.outputEncoding, 92));
  processor->renderGate = &gate; gate.arm();
  struct Probe {
    App &app; Processor *processor; const float *pixels;
    std::mutex mutex; std::condition_variable cv;
    bool observed = false, intact = false;
  } probe{app, processor, oldFull};
  RenderRuntimeTestAccess::observe(app.renderer, &probe, [](void *context, auto event) noexcept {
    if (event != RenderRuntimeTestAccess::Event::MutationWaiting) return;
    auto &probe = *static_cast<Probe *>(context);
    std::lock_guard<std::mutex> lock(probe.mutex);
    const auto *old = findNode(probe.app, NodeId{"transfer-borrowed"});
    probe.intact = old && old->processor.get() == probe.processor && probe.app.full.px.data() == probe.pixels;
    probe.observed = true; probe.cv.notify_all();
  });
  GateWorkerGuard guard{app, gate};
  if (exporting) CHECK(startExport(app, output));
  app.renderer.start(); CHECK(gate.wait(1));
  bool waited = false;
  std::thread release([&] {
    std::unique_lock<std::mutex> lock(probe.mutex);
    waited = probe.cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe.observed; }) && probe.intact;
    lock.unlock(); gate.unblock();
  });
  const bool applied = append ? appendGrade(app, grade) : applyGrade(app, grade);
  release.join(); app.renderer.joinExport();
  CHECK(applied && waited && idle(app) && app.full.px.data() == oldFull && app.preview == oldPreview);
  CHECK(app.nodes.size() == (append ? 3u : 2u));
  const auto frame = publication(app);
  CHECK(frame.result.state == EvaluationState::Available && frame.result.requested == app.documentOutput);
  // Source red=.25. Replace: .25*2=.5. Append: (.25*2)*2=1.
  CHECK(std::get<ImageValue>(frame.result.value)->px[0] == (append ? 1.0f : 0.5f));
  if (exporting) {
    Image exported, reference; ColorEncoding detected; bool raw;
    CHECK(loadImage(output, exported, detected, raw) && loadImage(expected, reference, detected, raw));
    CHECK(exported.w == 2 && exported.h == 1 && exported.px == reference.px);
  }
  return true;
}
static bool testTransferLifetimes() {
  for (bool append : {false, true}) for (bool exporting : {false, true})
    if (!testTransferLifetime(append, exporting)) return false;
  return true;
}

static bool testFragmentCaptureOwnership(bool exporting, bool failing) {
  Files files; Gate gate; App app; source(app);
  const NodeId id{"capture-borrowed"};
  auto *processor = numeric(app, id, 2.0f); CHECK(processor);
  processor->captureRuntime = &app.renderer;
  processor->renderGate = &gate;
  CHECK(selectNode(app, id));
  app.nodePositions[id.value] = {7.5, -3}; app.paramFilter[0] = 'g';
  const auto *pixels = app.full.px.data(); const auto preview = app.preview;
  const auto output = app.documentOutput;
  struct Probe {
    App &app;
    std::mutex mutex; std::condition_variable cv;
    bool waiting = false, completed = false, quiet = false;
  } probe{app};
  RenderRuntimeTestAccess::observe(app.renderer, &probe, [](void *context, auto event) noexcept {
    auto &probe = *static_cast<Probe *>(context);
    std::lock_guard<std::mutex> lock(probe.mutex);
    if (event == RenderRuntimeTestAccess::Event::MutationWaiting) {
      probe.waiting = true; probe.cv.notify_all();
    }
    if (event == RenderRuntimeTestAccess::Event::MutationCompleted) {
      const auto state = RenderRuntimeTestAccess::duringObservation(probe.app.renderer);
      probe.completed = true;
      probe.quiet = state.mutationDepth == 1 && !state.previewBusy && !state.exportBusy &&
                    state.previewPending && state.quietPending;
    }
  });
  GateWorkerGuard guard{app, gate};
  app.renderer.start(); CHECK(idle(app));
  // An idle read-only capture must create no render demand or new publication.
  PersistFragment captured; std::vector<std::string> warnings;
  const auto calls = processor->calls.load(), generation = publication(app).generation;
  CHECK(captureNodeFragment(app, {id}, captured, warnings));
  auto state = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(!state.previewPending && !state.quietPending && !state.previewBusy && state.mutationDepth == 0);
  CHECK(processor->calls == calls && publication(app).generation == generation && !probe.quiet);
  probe.completed = false;

  processor->failCapture = failing;
  gate.arm();
  const auto exportPath = files.path("capture.png");
  if (exporting) CHECK(startExport(app, exportPath)); else app.renderer.requestDocumentPreview();
  CHECK(gate.wait(1) && processor->rendering);
  state = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(exporting ? state.exportBusy : state.previewBusy);
  bool waited = false;
  std::thread release([&] {
    std::unique_lock<std::mutex> lock(probe.mutex);
    waited = probe.cv.wait_for(lock, std::chrono::seconds(2), [&] { return probe.waiting; });
    lock.unlock(); gate.unblock();
  });
  warnings = {"untouched"};
  const bool capturedOk = captureNodeFragment(app, {id}, captured, warnings);
  const auto status = app.getStatus();
  release.join(); app.renderer.joinExport();
  CHECK(capturedOk == !failing && waited && probe.completed && probe.quiet);
  CHECK(!processor->unsafeCapture && !processor->unsafeSync && processor->syncs == 2 && processor->captures == 2 && idle(app));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).mutationDepth == 0);
  CHECK(app.getStatus() == status); // Quiet recovery preserves export/capture status.
  CHECK(captured.nodes.size() == 1 && captured.nodes[0].id == id.value && captured.nodes[0].paramsJson.at("gain") == "2");
  CHECK(captured.positions.at(id.value).x == 7.5 && captured.positions.at(id.value).y == -3);
  CHECK(failing ? warnings == std::vector<std::string>{"untouched"} : warnings.empty());
  CHECK(app.selectedNodeId() == id && app.paramFilter[0] == 'g' && app.documentOutput == output);
  CHECK(app.full.px.data() == pixels && app.preview == preview && findNode(app, id)->processor.get() == processor);
  CHECK(findNode(app, id)->fixedInputs.at(ports::image) == image(app.primarySource));
  CHECK(publication(app).result.state == EvaluationState::Available && std::get<ImageValue>(publication(app).result.value)->px[0] == 0.5f);
  if (exporting) {
    const auto expected = files.path("expected.png"); auto reference = app.full;
    for (size_t i = 0; i < reference.px.size(); ++i) if (i % 4 != 3) reference.px[i] *= 2;
    CHECK(writeImage(reference, expected, app.outputEncoding, 92));
    Image actual, wanted; ColorEncoding encoding; bool raw;
    CHECK(loadImage(exportPath, actual, encoding, raw) && loadImage(expected, wanted, encoding, raw));
    CHECK(actual.w == 2 && actual.h == 1 && actual.px == wanted.px);
  }
  // Failed capture releases ownership, and a later capture/render succeeds.
  processor->failCapture = false;
  CHECK(captureNodeFragment(app, {id}, captured, warnings) && !processor->unsafeCapture);
  app.renderer.requestDocumentPreview(); CHECK(idle(app) && app.getStatus() == "1×1 preview");
  return true;
}
static bool testFragmentCaptures() {
  for (bool exporting : {false, true}) for (bool failing : {false, true})
    if (!testFragmentCaptureOwnership(exporting, failing)) return false;
  return true;
}

static bool testPresentationGate(bool refresh) {
  Executor executor; App app; source(app);
  const NodeId effect{"presentation-image"}, maskNode{"presentation-mask"};
  auto *processor = numeric(app, effect); CHECK(processor);
  CHECK(add(app, maskNode, NodeOperation::TestImageToMask));
  CHECK(graph::connect(app, image(app.primarySource), input(maskNode)).ok());
  struct Probe {
    App &app;
    Gate gate;
    std::mutex mutex;
    bool checkNext = false, stale = false;
    int generation = 0;
    std::vector<unsigned char> bytes;
  } probe{app};
  RenderRuntimeTestAccess::evaluation(app.renderer, &executor, [](void *context) {
    auto &probe = *static_cast<Probe *>(context);
    std::lock_guard<std::mutex> lock(probe.mutex);
    if (!probe.checkNext) return;
    std::lock_guard<std::mutex> displayLock(probe.app.displayMutex);
    probe.stale = probe.app.processed.generation != probe.generation || probe.app.displayRGBA != probe.bytes;
    probe.checkNext = false;
  }, &probe);
  RenderRuntimeTestAccess::publication(app.renderer, [](void *context, const OutputRef &) {
    static_cast<Probe *>(context)->gate.hold(); // Conversion is finished, commit has not begun.
  }, &probe);
  GateWorkerGuard guard{app, probe.gate}; app.renderer.start(); app.renderer.requestDocumentPreview();
  CHECK(idle(app));
  const auto calls = processor->calls.load();
  {
    std::lock_guard<std::mutex> lock(app.displayMutex);
    probe.generation = app.processed.generation; probe.bytes = app.displayRGBA;
  }
  { std::lock_guard<std::mutex> lock(app.colorMutex); app.outputEncoding.gamma = TransferFunction::Linear; }
  probe.gate.arm();
  if (refresh) app.renderer.requestDisplayRefresh(); else app.renderer.requestPreview();
  CHECK(probe.gate.wait(1));
  { std::lock_guard<std::mutex> lock(probe.mutex); probe.checkNext = true; }
  std::mutex responseMutex; std::condition_variable responseCv;
  bool responded = false, responsive = false;
  std::thread release([&] {
    std::unique_lock<std::mutex> lock(responseMutex);
    responsive = responseCv.wait_for(lock, std::chrono::seconds(2), [&] { return responded; });
    lock.unlock(); probe.gate.unblock();
  });
  const bool editable = app.renderer.canEditParameters();
  app.renderer.requestInspection(mask(maskNode));
  { std::lock_guard<std::mutex> lock(responseMutex); responded = true; } responseCv.notify_all();
  release.join();
  CHECK(responsive && editable && idle(app));
  { std::lock_guard<std::mutex> lock(probe.mutex); CHECK(!probe.stale && !probe.checkNext); }
  CHECK(processor->calls == calls + (refresh ? 0 : 1));
  CHECK(publication(app).result.requested == mask(maskNode));
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA == std::vector<unsigned char>({64,64,64,255})); }
  app.renderer.requestDocumentPreview(); CHECK(idle(app));
  CHECK(publication(app).result.requested == image(effect) && processor->calls == calls + (refresh ? 1 : 2));
  std::vector<unsigned char> expected; toDisplayRGBA8(*app.preview, app.outputEncoding, expected);
  { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA == expected); }
  return true;
}
static bool testPresentationResponsiveness() { return testPresentationGate(false) && testPresentationGate(true); }

static bool testNoImageGuidance() {
  Executor executor; App app;
  const std::string guidance = "Open a photo to begin."; app.setStatus(guidance);
  std::atomic<int> evaluations{0};
  RenderRuntimeTestAccess::evaluation(app.renderer, nullptr, [](void *context) {
    ++*static_cast<std::atomic<int> *>(context);
  }, &evaluations);
  WorkerGuard guard{app, executor}; app.renderer.start();
  CHECK(addNativeExposureNode(app) && idle(app));
  CHECK(setNodeParameterValue(app, *app.selectedNodeId(), "exposure", 1.0) && idle(app));
  CHECK(resetNodeParameter(app, *app.selectedNodeId(), "exposure") && idle(app));
  CHECK(app.getStatus() == guidance && evaluations == 0 && publication(app).generation == 0);
  clearNodes(app); CHECK(idle(app));
  CHECK(app.getStatus() == guidance && evaluations == 0 && publication(app).generation == 0);
  return true;
}

static bool testQuietOutcomes() {
  for (int outcome = 0; outcome < 5; ++outcome) {
    Files files; Executor executor; App app; source(app);
    const NodeId effect{"quiet-export"}, maskNode{"quiet-mask"};
    CHECK(numeric(app, effect) && add(app, maskNode, NodeOperation::TestImageToMask));
    CHECK(graph::connect(app, image(app.primarySource), input(maskNode)).ok());
    struct Fault { std::atomic<int> attempts{0}, failAt{-1}; } fault;
    RenderRuntimeTestAccess::evaluation(app.renderer, &executor, [](void *context) {
      auto &fault = *static_cast<Fault *>(context);
      if (++fault.attempts == fault.failAt) throw std::bad_alloc();
    }, &fault);
    WorkerGuard guard{app, executor}; app.renderer.start(); app.renderer.requestInspection(mask(maskNode));
    CHECK(idle(app) && publication(app).result.state == EvaluationState::Available);
    const auto before = publication(app).generation;
    {
      std::lock_guard<std::mutex> lock(executor.mutex);
      if (outcome == 1) executor.state = EvaluationState::Error;
      if (outcome == 2) executor.state = EvaluationState::Unavailable;
      if (outcome == 4) executor.cancelled = true;
    }
    if (outcome == 3) fault.failAt = fault.attempts + 2; // Export succeeds; quiet preview throws outside evaluator.
    CHECK(runExportJob(app, files.path("quiet.png")) && idle(app));
    CHECK(std::filesystem::exists(files.path("quiet.png")));
    const auto frame = publication(app);
    if (outcome == 0 || outcome == 4) {
      CHECK(app.getStatus().find("Exported quiet.png") == 0);
      CHECK(frame.result.state == EvaluationState::Available);
      CHECK(outcome == 0 ? frame.generation > before : frame.generation == before);
    } else {
      const auto state = outcome == 2 ? EvaluationState::Unavailable : EvaluationState::Error;
      CHECK(frame.result.state == state && frame.result.valueType == ValueType::Mask);
      CHECK(frame.generation > before && frame.generation == RenderRuntimeTestAccess::snapshot(app.renderer).epoch);
      CHECK(frame.result.requested == mask(maskNode) && !frame.result.diagnostics.empty());
      CHECK(app.getStatus().find(outcome == 2 ? "Preview unavailable" : "Render failed") == 0);
      CHECK(std::holds_alternative<std::monostate>(frame.result.value));
      { std::lock_guard<std::mutex> lock(app.displayMutex); CHECK(app.displayRGBA.empty() && app.display.w == 0); }
    }
    app.renderer.requestDocumentPreview(); CHECK(idle(app));
    CHECK(publication(app).result.state == EvaluationState::Available && app.getStatus() == "1×1 preview");
  }
  return true;
}

static bool testOverDepthWorker() {
  App app; source(app);
  depth_test::Chain chain(kMaxGraphDependencyDepth + 1, NodeOperation::ImageProcessor);
  app.nodes = std::move(chain.nodes); app.primarySource = chain.source; app.documentOutput = chain.output;
  app.renderer.requestPreview(); app.renderer.start(); CHECK(idle(app));
  const auto failed = publication(app);
  CHECK(failed.result.state == EvaluationState::Error);
  CHECK(std::holds_alternative<std::monostate>(failed.result.value));
  CHECK(failed.result.diagnostics.size() == 1 && depth_test::depthError(failed.result.diagnostics.front().message));
  {
    document_detail::DocumentMutation mutation(app);
    app.nodes.pop_back(); app.documentOutput = image(app.nodes.back().id);
    mutation.changed();
  }
  CHECK(idle(app) && publication(app).generation > failed.generation);
  CHECK(publication(app).result.state == EvaluationState::Available);
  return true;
}

static bool testUnchangedCommandsRetainDemand() {
  Files files;
  for (int demand = 0; demand < 4; ++demand) {
    Gate gate;
    struct Probe { Gate &gate; int mutations = 0; } probe{gate};
    App app; source(app);
    const NodeId effect{"effect"}, spare{"spare"}, slots{"slots"}, absent{"absent"};
    auto *processor = numeric(app, effect); CHECK(processor);
    processor->renderGate = &gate;
    CHECK(add(app, spare, NodeOperation::ImageProcessor) && add(app, slots, NodeOperation::TestSlotted));
    const NodeId invalidSelection{"mask-only-selection"};
    CHECK(add(app, invalidSelection, NodeOperation::TestReferenceMask));
    const auto first = graph::addSlot(app, slots).addedSlot;
    const auto second = graph::addSlot(app, slots).addedSlot;
    CHECK(first && second && selectNode(app, effect));
    OutputRef deepTail = image(app.primarySource);
    for (size_t i = 1; i < kMaxGraphDependencyDepth; ++i) {
      Node node; node.id = depth_test::id(i); node.enabled = false;
      node.fixedInputs.at(ports::image) = deepTail;
      deepTail = image(node.id); app.nodes.push_back(std::move(node));
    }
    app.paramFilter[0] = 'e';
    RenderRuntimeTestAccess::observe(app.renderer, &probe, [](void *context, auto event) noexcept {
      auto &probe = *static_cast<Probe *>(context);
      if (event == RenderRuntimeTestAccess::Event::MutationWaiting) {
        // A regression must fail without hanging the test in beginMutation.
        ++probe.mutations;
        probe.gate.unblock();
      }
      if (event == RenderRuntimeTestAccess::Event::MutationCompleted) ++probe.mutations;
    });
    GateWorkerGuard guard{app, gate};
    app.renderer.requestInspection(image(effect));
    if (demand != 1) { app.renderer.start(); CHECK(idle(app)); }
    if (demand == 2) { gate.arm(); app.renderer.requestInspection(image(effect)); CHECK(gate.wait(1)); }
    if (demand == 3) { gate.arm(); CHECK(startExport(app, files.path("unchanged.png")) && gate.wait(1)); }
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    const auto frame = publication(app);
    const int calls = processor->calls;
    Node duplicate; duplicate.id = effect;
    CHECK(depth_test::depthError(graph::connect(app, deepTail, input(spare)).error));
    CHECK(depth_test::depthError(graph::reconnect(app, deepTail, input(effect)).error));
    CHECK(!findNode(app, spare)->fixedInputs.at(ports::image));
    CHECK(!graph::addNode(app, std::move(duplicate)).ok() && duplicate.id == effect);
    CHECK(!graph::connect(app, image(app.primarySource), input(effect)).ok());
    CHECK(!graph::reconnect(app, image(effect), input(effect)).ok()); // Cycle.
    CHECK(!graph::disconnect(app, input(absent)).ok());
    CHECK(!graph::deleteNode(app, app.primarySource).ok());
    CHECK(!graph::deleteImageNode(app, app.primarySource).ok());
    CHECK(!graph::setDocumentOutput(app, mask(effect)).ok());
    CHECK(!graph::addSlot(app, effect).ok());
    CHECK(!graph::deleteSlot(app, slots, SlotId{999}).ok());
    CHECK(!graph::reorderSlot(app, slots, SlotId{999}, first).ok());
    Node invalid(NodeOperation::TestSlotted); invalid.id = NodeId{"invalid-splice"};
    CHECK(!graph::insertImageNode(app, std::move(invalid)).ok() && invalid.id == NodeId{"invalid-splice"});
    CHECK(!graph::moveImageNode(app, spare, effect).ok());
    CHECK(graph::reconnect(app, image(app.primarySource), input(effect)).ok());
    CHECK(graph::disconnect(app, input(spare)).ok()); // Already disconnected.
    CHECK(graph::setDocumentOutput(app, app.documentOutput).ok());
    CHECK(graph::moveImageNode(app, effect, effect).ok());
    CHECK(graph::moveImageNode(app, effect).ok()); // Already last.
    CHECK(graph::reorderSlot(app, slots, *first, first).ok());
    CHECK(graph::reorderSlot(app, slots, *first, second).ok()); // Already before.
    CHECK(graph::reorderSlot(app, slots, *second).ok()); // Already last.
    PersistFragment fragment; std::vector<std::string> warnings{"unchanged"};
    for (const auto &ids : {std::vector<NodeId>{}, std::vector<NodeId>{app.primarySource},
                           std::vector<NodeId>{effect, absent}, std::vector<NodeId>{effect, effect}})
      CHECK(!captureNodeFragment(app, ids, fragment, warnings));
    CHECK(fragment.nodes.empty() && warnings == std::vector<std::string>{"unchanged"});
    for (auto selection : {std::optional<NodeId>{}, std::optional<NodeId>{app.primarySource},
                           std::optional<NodeId>{absent}, std::optional<NodeId>{slots}}) {
      app.selection = selection ? InspectorSelection{*selection} : InspectorSelection{};
      CHECK(!canCopySelectedNode(app) && !copySelectedNode(app));
    }
    // Invalid Add selections must not drain pending/active preview or export.
    for (const auto &id : {absent, invalidSelection, deepTail.nodeId}) {
      app.selection = id;
      CHECK(!addNativeExposureNode(app) && !addNativeCstNode(app) && !addCtlNode(app, "not-constructed.ctl"));
      CHECK(app.selectedNodeId() == id && app.paramFilter[0] == 'e');
    }
    app.selection = NodeId{effect};
    app.path = files.path("protected.nef"); app.sidecarWriteBlockedPath = app.path;
    CHECK(!canCopySelectedNode(app) && !copySelectedNode(app));
    CHECK(!captureNodeFragment(app, {effect}, fragment, warnings));
    app.sidecarWriteBlockedPath.clear();
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(probe.mutations == 0 && after.epoch == before.epoch && after.mutationDepth == 0);
    CHECK(after.previewPending == before.previewPending && after.quietPending == before.quietPending &&
          after.previewBusy == before.previewBusy && after.exportBusy == before.exportBusy &&
          after.displayPending == before.displayPending &&
          processor->calls == calls);
    CHECK(publication(app).generation == frame.generation && app.selectedNodeId() == effect && app.paramFilter[0] == 'e');
    CHECK(app.documentOutput == image(effect) && findNode(app, effect)->processor.get() == processor &&
          findNode(app, effect)->fixedInputs.at(ports::image) == image(app.primarySource));
    gate.unblock();
    if (demand == 3) app.renderer.joinExport();
    if (demand == 1) app.renderer.start();
    CHECK(idle(app) && publication(app).result.requested == image(effect));
    // A genuine change still owns exclusion and publishes a new graph result.
    const auto generation = publication(app).generation;
    CHECK(graph::setDocumentOutput(app, image(app.primarySource)).ok());
    CHECK(probe.mutations == 1 && idle(app) && publication(app).generation > generation);
  }
  return true;
}

static bool testIdenticalSlotReorder() {
  for (bool connected : {false, true}) {
    App app; source(app);
    CHECK(addNativeExposureNode(app));
    const NodeId owner{"ordered-slots"};
    CHECK(add(app, owner, NodeOperation::TestSlotted));
    const auto first = graph::addSlot(app, owner).addedSlot;
    const auto second = graph::addSlot(app, owner).addedSlot;
    CHECK(first && second && *first != *second);
    if (connected) {
      CHECK(graph::connect(app, image(app.primarySource), {owner, first, ports::image}).ok());
      CHECK(graph::connect(app, image(app.primarySource), {owner, second, ports::image}).ok());
    }
    // Declared inputs are either all disconnected or have the same producer;
    // settings also match. SlotId order alone must make this a real edit.
    const auto bindings = findNode(app, owner)->slots.front().inputs;
    CHECK(bindings == findNode(app, owner)->slots.back().inputs);
    app.renderer.requestDocumentPreview(); app.renderer.start(); CHECK(idle(app));
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    const auto frame = publication(app);
    CHECK(graph::reorderSlot(app, owner, *second, first).ok());
    const auto &slots = findNode(app, owner)->slots;
    CHECK(slots.size() == 2 && slots[0].id == *second && slots[1].id == *first);
    CHECK(slots[0].inputs == bindings && slots[1].inputs == bindings &&
          slots[0].settings.enabled && slots[1].settings.enabled);
    CHECK(documentStateError(app).empty() && idle(app));
    CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch > before.epoch &&
          publication(app).generation > frame.generation && publication(app).result.state == EvaluationState::Available);
    const auto unchanged = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(graph::reorderSlot(app, owner, *second, first).ok()); // Already before.
    CHECK(graph::reorderSlot(app, owner, *first).ok()); // Already last.
    CHECK(graph::reorderSlot(app, owner, *second, second).ok()); // Same Slot.
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(after.epoch == unchanged.epoch && after.mutationDepth == 0 &&
          !after.previewPending && !after.previewBusy && !after.displayPending);
    CHECK(findNode(app, owner)->slots[0].id == *second && findNode(app, owner)->slots[1].id == *first);
  }
  return true;
}

static bool testLegacyRefusalAndGraphSave() {
  Files files; App app; source(app);
  CHECK(addNativeExposureNode(app));
  app.path = files.path("source.nef");
  saveCurrentInputSidecar(app);
  // Find the current sidecar naming rule without assuming a transfer format.
  std::filesystem::path stored;
  for (const auto &entry : std::filesystem::directory_iterator(files.dir))
    if (entry.path().extension() == ".json") stored = entry.path();
  CHECK(!stored.empty());
  std::ifstream beforeFile(stored); const std::string before((std::istreambuf_iterator<char>(beforeFile)), {});
  CHECK(add(app, NodeId{"off-spine"}, NodeOperation::ImageProcessor));
  findNode(app, NodeId{"off-spine"})->storedBackend = "ofx";
  findNode(app, NodeId{"off-spine"})->storedIdentifier = "unavailable.off-spine";
  bool refused = false;
  try { (void)captureChain(app); } catch (const std::runtime_error &) { refused = true; }
  CHECK(refused);
  CHECK(saveCurrentInputSidecar(app));
  std::ifstream afterFile(stored); const std::string after((std::istreambuf_iterator<char>(afterFile)), {});
  PersistFile saved; std::string error;
  CHECK(before != after && decodeGraphFile(after, saved, error) && saved.grade && saved.grade->graph.nodes.size() == 3);
  CHECK(saved.grade->graph.nodes[2].id == "off-spine" && !saved.grade->graph.nodes[2].inputs.at(ports::image));
  return true;
}

// These tests call only the shared parameter API: no UI completion helper and
// no explicit preview request after an edit. Binary-exact gains and inputs make
// the expected RGB multiplication independent of backend/render implementation.
static bool pixels(App &app, float gain) {
  const auto frame = publication(app);
  if (frame.result.state != EvaluationState::Available) return false;
  const auto *value = std::get_if<ImageValue>(&frame.result.value);
  if (!value || !*value) return false;
  auto expected = *app.preview;
  for (size_t i = 0; i < expected.px.size(); ++i) if (i % 4 != 3) expected.px[i] *= gain;
  return (*value)->w == expected.w && (*value)->h == expected.h && (*value)->px == expected.px;
}

static bool testParameterPreview() {
  App app; source(app);
  app.preview = std::make_shared<Image>(Image{{-0.25f, 0.5f, 1.25f, 0.75f}, 1, 1});
  CHECK(addNativeExposureNode(app));
  const auto effect = *app.selectedNodeId();
  // No worker can advance the epoch here: exactly one request per acceptance,
  // even when the backend accepts the same value it already holds.
  for (int i = 0; i < 2; ++i) {
    const int before = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
    CHECK(setNodeParameterValue(app, effect, "exposure", 1.0));
    CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == before + 1);
  }
  const auto beforeRestore = RenderRuntimeTestAccess::snapshot(app.renderer);
  auto restored = document_detail::restoreGraph(captureGraph({app.nodes, app.primarySource, app.documentOutput}));
  CHECK(restored.nodes.size() == app.nodes.size() &&
        RenderRuntimeTestAccess::snapshot(app.renderer).epoch == beforeRestore.epoch);
  app.renderer.start(); CHECK(idle(app) && pixels(app, 2.0f));
  for (int i = 0; i < 2; ++i) {
    const int before = publication(app).generation;
    CHECK(resetNodeParameter(app, effect, "exposure") && idle(app));
    CHECK(publication(app).generation > before && pixels(app, 1.0f));
  }
  // Editing an inspected upstream processor must not switch back to Output.
  const NodeId downstream{"downstream"}; CHECK(numeric(app, downstream, 8.0f));
  app.renderer.requestInspection(image(effect)); CHECK(idle(app));
  const auto before = publication(app).generation;
  CHECK(setNodeParameterValue(app, effect, "exposure", 2.0) && idle(app));
  const auto frame = publication(app);
  CHECK(frame.generation > before && frame.request && *frame.request == image(effect));
  CHECK(frame.result.requested == image(effect) && app.documentOutput == image(downstream) && pixels(app, 4.0f));
  return true;
}

static std::string parameterCtl(Files &files) {
  const auto path = files.path("ParameterGain.ctl");
  std::ofstream script(path);
  script << "void main(input varying float rIn, input varying float gIn, input varying float bIn,\n"
            " output varying float rOut, output varying float gOut, output varying float bOut,\n"
            " output varying float aOut, input varying float aIn = 1.0, input uniform float gain = 1.0)\n"
            "{ rOut = rIn * gain; gOut = gIn * gain; bOut = bIn * gain; aOut = aIn; }\n";
  return path;
}

static bool testParameterDuringPreview(bool ctl) {
  Files files; Gate gate; App app; source(app);
  CHECK(ctl ? addCtlNode(app, parameterCtl(files)) : addNativeExposureNode(app));
  const auto effect = *app.selectedNodeId();
  const auto parameter = ctl ? "gain" : "exposure";
  const NodeId downstream{"parameter-barrier"};
  auto *processor = numeric(app, downstream); CHECK(processor);
  processor->renderGate = &gate;
  struct Probe { App &app; int evaluations = 0, baseline = 0; bool stale = false; } probe{app};
  RenderRuntimeTestAccess::evaluation(app.renderer, nullptr, [](void *context) {
    auto &probe = *static_cast<Probe *>(context);
    if (++probe.evaluations == 3) probe.stale = publication(probe.app).generation != probe.baseline;
  }, &probe);
  GateWorkerGuard guard{app, gate}; app.renderer.start(); CHECK(idle(app) && pixels(app, 1.0f));
  probe.baseline = publication(app).generation;
  gate.arm();
  CHECK(setNodeParameterValue(app, effect, parameter, ctl ? 2.0 : 1.0) && gate.wait(1));
  // The old numerical result is now held downstream of the real backend.
  // An independent release path makes an accidental DocumentMutation wait fail
  // the responsiveness check instead of deadlocking the suite.
  std::mutex mutex; std::condition_variable cv;
  bool completed = false, responsive = false;
  std::thread release([&] {
    std::unique_lock<std::mutex> lock(mutex);
    responsive = cv.wait_for(lock, std::chrono::seconds(2), [&] { return completed; });
    lock.unlock(); gate.unblock();
  });
  const bool accepted = setNodeParameterValue(app, effect, parameter, ctl ? 4.0 : 2.0);
  { std::lock_guard<std::mutex> lock(mutex); completed = true; } cv.notify_one();
  release.join();
  CHECK(accepted && responsive && idle(app) && pixels(app, 4.0f));
  CHECK(probe.evaluations == 3 && !probe.stale && publication(app).generation > probe.baseline);
  const auto before = publication(app).generation;
  CHECK(resetNodeParameter(app, effect, parameter) && idle(app));
  CHECK(publication(app).generation > before && pixels(app, 1.0f));
  return true;
}
static bool testParameterPreviewSupersession() {
  return testParameterDuringPreview(false) && testParameterDuringPreview(true);
}

static bool testRejectedParameterEdits() {
  Files files;
  for (int demand = 0; demand < 4; ++demand) {
    Gate gate; App app; source(app);
    CHECK(addNativeExposureNode(app)); const auto effect = *app.selectedNodeId();
    CHECK(setNodeParameterValue(app, effect, "exposure", 1.0));
    CHECK(addNativeExposureNode(app)); const auto stale = *app.selectedNodeId();
    CHECK(destroyNode(app, stale));
    const NodeId missing{"missing"}; CHECK(add(app, missing, NodeOperation::ImageProcessor));
    const NodeId barrier{"rejected-parameter-barrier"};
    auto *processor = numeric(app, barrier); CHECK(processor); processor->renderGate = &gate;
    GateWorkerGuard guard{app, gate};
    if (demand != 1) { app.renderer.start(); CHECK(idle(app) && pixels(app, 2.0f)); }
    if (demand == 2) { gate.arm(); app.renderer.requestPreview(); CHECK(gate.wait(1)); }
    const auto output = files.path("parameter-export.png");
    if (demand == 3) { gate.arm(); CHECK(startExport(app, output) && gate.wait(1)); }
    const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
    const auto frame = publication(app);
    const auto calls = processor->calls.load();
    auto rejectAll = [&](const NodeId &id) {
      return !setNodeParameterValue(app, id, "exposure", 2.0) &&
             !resetNodeParameter(app, id, "exposure") && !activateNodeParameter(app, id, "exposure");
    };
    CHECK(rejectAll(NodeId{"invalid"}) && rejectAll(stale) && rejectAll(missing) && rejectAll(app.primarySource));
    CHECK(!setNodeParameterValue(app, effect, "invalid", 1.0));
    CHECK(!resetNodeParameter(app, effect, "invalid") && !activateNodeParameter(app, effect, "exposure"));
    CHECK(!setNodeParameterValue(app, effect, "exposure", true));
    CHECK(!setNodeParameterValue(app, effect, "exposure", std::numeric_limits<double>::quiet_NaN()));
    app.path = files.path("protected.nef"); app.sidecarWriteBlockedPath = app.path;
    CHECK(rejectAll(effect)); app.sidecarWriteBlockedPath.clear();
    if (demand == 3) CHECK(rejectAll(effect));
    const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(after.epoch == before.epoch && after.previewPending == before.previewPending &&
          after.previewBusy == before.previewBusy && after.quietPending == before.quietPending &&
          after.displayPending == before.displayPending && after.exportBusy == before.exportBusy &&
          after.mutationDepth == 0 && processor->calls == calls && publication(app).generation == frame.generation);
    CHECK(std::get<double>(findNode(app, effect)->processor->parameters()[0].value) == 1.0);
    gate.unblock();
    if (demand == 3) {
      app.renderer.joinExport();
      auto expected = app.full;
      for (size_t i = 0; i < expected.px.size(); ++i) if (i % 4 != 3) expected.px[i] *= 2.0f;
      const auto reference = files.path("parameter-reference.png");
      CHECK(writeImage(expected, reference, app.outputEncoding, 92));
      Image exported, decoded; ColorEncoding encoding; bool raw;
      CHECK(loadImage(output, exported, encoding, raw) && loadImage(reference, decoded, encoding, raw));
      CHECK(exported.w == expected.w && exported.h == expected.h && exported.px == decoded.px);
    }
    if (demand == 1) app.renderer.start();
    CHECK(idle(app) && pixels(app, 2.0f));
    const auto generation = publication(app).generation;
    CHECK(setNodeParameterValue(app, effect, "exposure", 2.0) && idle(app));
    CHECK(publication(app).generation > generation && pixels(app, 4.0f));
  }
  return true;
}

// An in-process plugin exercises the real OFX adapter's notification and button
// semantics without depending on installed plugins or changing backend policy.
struct ParameterOfxFixture {
  OfxPlugin plugin{};
  int index = (int)gPlugins.size();
  std::vector<std::string> notifications;
  std::vector<std::string> changedParameters;
  bool userEdited = true;
  ParameterOfxFixture() {
    plugin.pluginIdentifier = "org.rawnode.selftest.parameter-edit";
    plugin.mainEntry = [](const char *action, const void *handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
      auto *effect = static_cast<Effect *>(const_cast<void *>(handle));
      auto &probe = *static_cast<ParameterOfxFixture *>(effect->props.m.at("parameter-probe")[0].p);
      if (!std::strcmp(action, kOfxActionBeginInstanceChanged) || !std::strcmp(action, kOfxActionInstanceChanged) ||
          !std::strcmp(action, kOfxActionEndInstanceChanged)) {
        probe.notifications.push_back(action);
        probe.userEdited &= sprop(*P(in), kOfxPropChangeReason) == kOfxChangeUserEdited;
        if (!std::strcmp(action, kOfxActionInstanceChanged))
          probe.changedParameters.push_back(sprop(*P(in), kOfxPropName));
        if (!std::strcmp(action, kOfxActionInstanceChanged) && sprop(*P(in), kOfxPropName) == "button") {
          std::lock_guard<std::mutex> lock(gValueMutex); findParam(effect, "gain")->v[0] = 4.0;
        }
        return kOfxStatOK;
      }
      if (std::strcmp(action, kOfxImageEffectActionRender)) return kOfxStatReplyDefault;
      double gain;
      { std::lock_guard<std::mutex> lock(gValueMutex); gain = findParam(effect, "gain")->v[0]; }
      for (int i = 0; i < effect->w * effect->h * 4; ++i)
        effect->dst[i] = effect->src[i] * (i % 4 == 3 ? 1.0f : (float)gain);
      return kOfxStatOK;
    };
    PluginEntry entry{}; entry.plugin = &plugin; entry.descriptor = std::make_unique<Effect>();
    propSetPointer(H(&entry.descriptor->props), "parameter-probe", 0, this);
    for (bool button : {false, true}) {
      auto param = std::make_unique<Param>();
      param->name = button ? "button" : "gain"; param->type = button ? kOfxParamTypePushButton : kOfxParamTypeDouble;
      param->v = {1.0}; propSetDouble(H(&param->props), kOfxParamPropDefault, 0, 1.0);
      entry.descriptor->params.push_back(std::move(param));
    }
    gPlugins.push_back(std::move(entry));
  }
  ~ParameterOfxFixture() { gPlugins.pop_back(); }
};

static bool testBackendInsertion() {
  Files files; ParameterOfxFixture ofx;
  for (bool ctl : {false, true}) {
    App app;
    CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
    CHECK(addNativeCstNode(app)); const auto b = *app.selectedNodeId();
    CHECK(selectNode(app, a));
    const auto create = [&] { return ctl ? addCtlNode(app, parameterCtl(files)) : addNode(app, ofx.index); };
    CHECK(create()); const auto inserted = *app.selectedNodeId();
    CHECK(selectedNode(app)->processor->backend() == (ctl ? ProcessorBackend::CTL : ProcessorBackend::OFX));
    CHECK(app.documentOutput == image(b) && findNode(app, b)->fixedInputs.at(ports::image) == image(inserted));
    CHECK(selectedNode(app)->fixedInputs.at(ports::image) == image(a));
    CHECK(selectedNode(app)->processor->setParameterValue("gain", 2.5));
    Processor *retained = selectedNode(app)->processor.get();
    // Disconnect the new processor from the main chain, preserving its own input.
    CHECK(graph::reconnect(app, image(a), input(b)).ok());
    CHECK(create()); const auto branch = *app.selectedNodeId();
    CHECK(app.documentOutput == image(b) && selectedNode(app)->fixedInputs.at(ports::image) == image(inserted));
    CHECK(findNode(app, inserted)->processor.get() == retained);
    CHECK(std::get<double>(retained->parameters()[0].value) == 2.5);
    CHECK(findNode(app, b)->fixedInputs.at(ports::image) == image(a));
    CHECK(selectNode(app, app.primarySource) && create()); const auto first = *app.selectedNodeId();
    CHECK(findNode(app, a)->fixedInputs.at(ports::image) == image(first));
    CHECK(selectedNode(app)->fixedInputs.at(ports::image) == image(app.primarySource));
    CHECK(findNode(app, branch)->fixedInputs.at(ports::image) == image(inserted));
    const auto count = app.nodes.size();
    const auto selection = app.selection;
    CHECK(!addCtlNode(app, files.path("absent.ctl")) && !addNode(app, -1));
    CHECK(app.nodes.size() == count && app.selection == selection && app.documentOutput == image(b));
  }
  return true;
}

static bool testOfxParameterScheduling() {
  ParameterOfxFixture fixture; App app; source(app);
  CHECK(addNode(app, fixture.index)); const auto effect = *app.selectedNodeId();
  int before = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
  CHECK(setNodeParameterValue(app, effect, "gain", 2.0));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == ++before);
  CHECK(resetNodeParameter(app, effect, "gain"));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == ++before);
  CHECK(activateNodeParameter(app, effect, "button"));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == ++before);
  CHECK(!setNodeParameterValue(app, effect, "gain", false) && !resetNodeParameter(app, effect, "button") &&
        !activateNodeParameter(app, effect, "gain"));
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == before && fixture.notifications.size() == 9);
  CHECK(fixture.userEdited && fixture.changedParameters == std::vector<std::string>({"gain", "gain", "button"}));
  for (size_t i = 0; i < fixture.notifications.size(); i += 3) {
    CHECK(fixture.notifications[i] == kOfxActionBeginInstanceChanged &&
          fixture.notifications[i + 1] == kOfxActionInstanceChanged && fixture.notifications[i + 2] == kOfxActionEndInstanceChanged);
  }
  app.renderer.start(); CHECK(idle(app) && pixels(app, 4.0f));
  return true;
}
} // namespace

bool testGraphRuntime() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"Strength edits supersede active preview without waiting; latest zero bypass publishes and export excludes edits", testStrengthEditOwnership},
      {"Native/CTL/OFX Add Processor shares serial, Source and off-chain insertion policy", testBackendInsertion},
      {"Over-depth worker returns Error, then evaluates a repaired boundary graph", testOverDepthWorker},
      {"Shared parameter set/reset schedule numerical previews and preserve inspection", testParameterPreview},
      {"Native/CTL parameter edits stay responsive during preview and supersede old output", testParameterPreviewSupersession},
      {"Rejected parameter edits preserve idle/pending/active/export demand and export pixels; editing recovers", testRejectedParameterEdits},
      {"OFX shared set/reset/activate schedule once, preserve notifications and render button state", testOfxParameterScheduling},
      {"6D creation, move/delete, derived capture order and V2 topology restore", testTopologyAndV2},
      {"6D typed Image/Mask/Unavailable/Error publication and cached display conversion", testTypedPublicationAndDisplay},
      {"Step 8 Inactive inspection clears stale pixels and quiet restoration preserves export status", testInactiveInspectionAndRestoration},
      {"6D Image-to-Mask and Mask-to-Image endpoint supersession", testEndpointSupersession},
      {"6D full-resolution graph export, reachable warnings, refusal and resolution context", testExportPolicyAndContext},
      {"6D allocation-style worker exception publishes Error and worker survives mutation", testWorkerExceptionBoundary},
      {"6D B1 async export retains Photo A while openPath waits to commit Photo B", testExportPhotoSwitch},
      {"6D I1 photo switching drains active preview and keeps Source dimensions coherent", testPreviewPhotoSwitch},
      {"Copy Node read-only capture drains active preview/export, recovers quietly and releases on failure", testFragmentCaptures},
      {"Step 7 Replace/Append drain borrowed preview/export nodes and pixels", testTransferLifetimes},
      {"6D I2 prepared preview/recolour remains responsive and rejects stale presentation", testPresentationResponsiveness},
      {"6D I3 no-image processor edits preserve initial guidance without evaluation", testNoImageGuidance},
      {"6D I4 quiet restoration reports current failures and preserves successful export status", testQuietOutcomes},
      {"Step 7 full graph save retains off-spine nodes while legacy capture refuses", testLegacyRefusalAndGraphSave},
      {"6E.2 rejected/no-op graph commands preserve idle/pending/active preview and active export", testUnchangedCommandsRetainDemand},
      {"6F T1 identical/disconnected Slot bindings retain real order changes and detect no-ops", testIdenticalSlotReorder},
  };
  for (const auto &test : cases) {
    if (!test.test()) { std::fprintf(stderr, "selftest FAILED: %s\n", test.name); return false; }
    std::printf("ok  %s\n", test.name);
  }
  return true;
}
