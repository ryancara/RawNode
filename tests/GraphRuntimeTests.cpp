#include "GraphRuntimeTests.h"

#include "AppState.h"
#include "DocumentMutation.h"
#include "Export.h"
#include "GraphCommands.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActions.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

class NumericProcessor final : public Processor {
 public:
  float gain = 1.0f, offset = 0.0f;
  std::atomic<int> calls{0}, width{0}, height{0};
  std::atomic<bool> fail{false};
  const float *lastInput = nullptr;
  ProcessorBackend backend() const override { return ProcessorBackend::Native; }
  std::string identifier() const override { return "test.numeric"; }
  std::string displayName() const override { return "Test numeric"; }
  std::vector<ProcessorParameter> parameters() const override { return {}; }
  bool setParameterValue(const std::string &, const ParameterValue &, bool) override { return false; }
  bool resetParameter(const std::string &, bool) override { return false; }
  bool activateParameter(const std::string &) override { return false; }
  void setRenderSize(int w, int h) override { width = w; height = h; }
  ProcessorResult render(const Image &in, Image &out, const RenderCancellation &) override {
    ++calls; lastInput = in.px.data();
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

static bool testTopologyAndV2() {
  App app; source(app);
  CHECK(addNativeExposureNode(app) && addNativeCstNode(app));
  const auto first = app.nodes[1].id, second = app.nodes[2].id;
  CHECK(app.nodes[1].fixedInputs.at(ports::image) == image(app.primarySource));
  CHECK(app.nodes[2].fixedInputs.at(ports::image) == image(first) && app.documentOutput == image(second));
  auto saved = captureChain(app);
  moveNode(app, 2, 1);
  CHECK(app.nodes[1].id == first && app.nodes[2].id == second && app.documentOutput == image(first));
  CHECK(app.nodes[1].fixedInputs.at(ports::image) == image(second) && app.selectedNode == 2);
  CHECK(captureChain(app).nodes[0].id == second.value);
  App restored; source(restored); applyChain(restored, captureChain(app));
  CHECK(restored.nodes[1].id == second && restored.nodes[2].id == first && restored.documentOutput == image(first));
  CHECK(restored.nodes[1].fixedInputs.at(ports::image) == image(restored.primarySource) &&
        restored.nodes[2].fixedInputs.at(ports::image) == image(second));
  destroyNode(app, 2);
  CHECK(app.documentOutput == image(first) && app.nodes[1].fixedInputs.at(ports::image) == image(app.primarySource));
  PersistNode missing; missing.id = "preserved-missing"; missing.backend = "dctl";
  missing.identifier = "asset.dctl"; missing.paramsJson["opaque"] = "[1,2]";
  saved.nodes.push_back(missing); saved.selectedNodeId = missing.id;
  applyChain(restored, saved);
  CHECK(restored.selectedNode == 3 && restored.documentOutput == image(NodeId{missing.id}));
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
  const int selected = ordered.selectedNode;
  moveNode(ordered, 2, 1);
  result = ordered.renderer.evaluateOwned(ordered.documentOutput, context, ordered.preview);
  CHECK(std::get<ImageValue>(result.value)->px[0] == 2.5f && ordered.nodes[1].id == a && ordered.nodes[2].id == b);
  CHECK(ordered.selectedNode == selected);
  destroyNode(ordered, 2);
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
  WorkerGuard guard{app, executor}; app.renderer.start(); app.renderer.requestDocumentPreview(); CHECK(idle(app));
  auto result = publication(app);
  CHECK(result.result.state == EvaluationState::Error && result.result.requested == image(endpoint) && processor->calls == 0);
  CHECK(result.request && *result.request == image(endpoint) && diagnostic(result.result, "bad_alloc"));
  { document_detail::DocumentMutation mutation(app); mutation.changed(); }
  CHECK(idle(app) && publication(app).result.state == EvaluationState::Available && processor->calls == 1 && attempts == 2);
  return true;
}

static bool testNarrowV2Refusal() {
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
  bool refused = false;
  try { (void)captureChain(app); } catch (const std::runtime_error &) { refused = true; }
  CHECK(refused);
  saveCurrentInputSidecar(app);
  std::ifstream afterFile(stored); const std::string after((std::istreambuf_iterator<char>(afterFile)), {});
  CHECK(before == after && app.getStatus().find("linear V2") != std::string::npos);
  return true;
}
} // namespace

bool testGraphRuntime() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"6D creation, move/delete, derived capture order and V2 topology restore", testTopologyAndV2},
      {"6D typed Image/Mask/Unavailable/Error publication and cached display conversion", testTypedPublicationAndDisplay},
      {"6D Image-to-Mask and Mask-to-Image endpoint supersession", testEndpointSupersession},
      {"6D full-resolution graph export, reachable warnings, refusal and resolution context", testExportPolicyAndContext},
      {"6D allocation-style worker exception publishes Error and worker survives mutation", testWorkerExceptionBoundary},
      {"6D narrow linear V2 refusal preserves the existing source sidecar", testNarrowV2Refusal},
  };
  for (const auto &test : cases) {
    if (!test.test()) { std::fprintf(stderr, "selftest FAILED: %s\n", test.name); return false; }
    std::printf("ok  %s\n", test.name);
  }
  return true;
}
