#include "GraphEvaluatorTests.h"

#include "GraphCommands.h"
#include "GraphEvaluator.h"
#include "GraphValidation.h"
#include "NodeGraph.h"
#include "RenderPipeline.h"
#include "processors/NativeExposureProcessor.h"
#include "processors/OfxProcessor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>

// Borrow only a test-owned epoch; no worker or scheduler controls are exposed.
class RenderCancellationTestAccess {
 public:
  static RenderCancellation token(const std::atomic<int> &epoch, int expected) {
    return RenderCancellation(epoch, expected);
  }
};

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::fprintf(stderr, "graph evaluator check failed at line %d: %s\n", __LINE__, #expression); \
    return false; \
  } \
} while (false)

namespace {

static OutputRef image(const NodeId &id) { return {id, ports::image}; }
static OutputRef mask(const NodeId &id) { return {id, ports::mask}; }
static InputAddress input(const NodeId &id, const PortId &port = ports::image,
                          std::optional<SlotId> slot = std::nullopt) { return {id, slot, port}; }
static ImageValue raster(int w, int h, float red = 0.25f) {
  auto value = std::make_shared<Image>();
  value->w = w; value->h = h;
  for (int i = 0; i < w * h; ++i) {
    value->px.insert(value->px.end(), {red, -0.5f, 2.0f, 0.375f});
  }
  return value;
}
static ImageValue pixels(const EvaluationResult &result) {
  const auto *value = std::get_if<ImageValue>(&result.value);
  return value ? *value : nullptr;
}
static MaskValue coverage(const EvaluationResult &result) {
  const auto *value = std::get_if<MaskValue>(&result.value);
  return value ? *value : nullptr;
}
static bool message(const EvaluationResult &result, const std::string &text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const EvaluationDiagnostic &d) {
    return d.message.find(text) != std::string::npos;
  });
}
static bool all(const MaskValue &value, float sample) {
  return value && std::all_of(value->px.begin(), value->px.end(), [&](float x) { return x == sample; });
}

struct Fixture {
  std::vector<Node> nodes;
  NodeId source{"source-root"};
  OutputRef output{source, ports::image};
  ImageValue sourcePixels = raster(2, 2);
  EvaluationContext context{EvaluationPurpose::Inspection, 2, 2, 2, 2};
  Fixture() {
    Node node(NodeOperation::Source); node.id = source;
    nodes.push_back(std::move(node));
  }
  graph::Document document() { return {nodes, source, output}; }
  GraphView view() const { return {nodes, source, output}; }
  bool add(const NodeId &id, NodeOperation op = NodeOperation::TestImage) {
    Node node(op); node.id = id;
    return graph::addNode(document(), std::move(node)).ok();
  }
  Node *node(const NodeId &id) {
    for (auto &node : nodes) if (node.id == id) return &node;
    return nullptr;
  }
  bool connect(const OutputRef &producer, const InputAddress &consumer) {
    return graph::connect(document(), producer, consumer).ok();
  }
  EvaluationResult evaluate(const OutputRef &endpoint, graph_test::Executor *executor = nullptr,
                            const RenderCancellation &cancellation = {}) const {
    return evaluateGraph(view(), endpoint, context, sourcePixels, cancellation, executor);
  }
};

// Toy arithmetic is confined here. None of these operations is a production
// Mask, Mixer, Strength or resampling feature. Configuration/counters are test
// owned, separate from the document vocabulary and evaluator policy.
struct ToySettings {
  float gain = 1.0f, add = 0.0f, maskValue = 0.25f;
  bool halfSize = false, foldSlots = false, exportMismatch = false;
  bool fail = false, throwException = false, wrongType = false, badShape = false;
  bool omitOutput = false, extraOutput = false;
  std::function<void()> duringExecution;
};
class ToyExecutor final : public graph_test::Executor {
 public:
  std::map<std::string, ToySettings> settings;
  std::map<std::string, int> calls;
  std::map<std::string, std::vector<SlotId>> slotOrder;
  std::map<std::string, EvaluationContext> contexts;
  std::vector<std::weak_ptr<const Image>> images;
  std::vector<std::weak_ptr<const Mask>> masks;

  graph_test::OperationResult execute(const Node &node, const EvaluationContext &context,
                                     const graph_test::InputValues &inputs, const RenderCancellation &) override {
    ++calls[node.id.value];
    contexts[node.id.value] = context;
    auto &config = settings[node.id.value];
    if (config.duringExecution) config.duringExecution();
    if (config.throwException) throw std::runtime_error("toy exception text");
    if (config.fail) return {EvaluationState::Error, {}, "toy execution failure"};
    graph_test::OperationResult result;
    switch (node.operation) {
      case NodeOperation::TestImage: {
        auto out = std::make_shared<Image>(*inputs.image);
        if (config.halfSize) {
          out->w = std::max(1, out->w / 2); out->h = std::max(1, out->h / 2);
          out->px.resize(static_cast<size_t>(out->w) * out->h * 4);
          for (int y = 0; y < out->h; ++y)
            for (int x = 0; x < out->w; ++x)
              for (int c = 0; c < 4; ++c)
                out->px[(y * out->w + x) * 4 + c] = inputs.image->px[(y * inputs.image->w + x) * 4 + c];
        }
        for (size_t i = 0; i < out->px.size(); ++i)
          if (i % 4 != 3) out->px[i] = out->px[i] * config.gain + config.add;
        images.push_back(out);
        result.outputs.emplace(ports::image, ImageValue(out));
        break;
      }
      case NodeOperation::TestImageToMask:
      case NodeOperation::TestReferenceMask:
      case NodeOperation::TestMaskProcessor:
      case NodeOperation::TestMultipleOutputs: {
        auto out = std::make_shared<Mask>();
        if (node.operation == NodeOperation::TestMaskProcessor) {
          out->w = inputs.mask->w; out->h = inputs.mask->h;
          for (float x : inputs.mask->px) out->px.push_back(1.0f - x);
        } else if (node.operation == NodeOperation::TestReferenceMask) {
          out->w = inputs.reference->w; out->h = inputs.reference->h;
          if (config.exportMismatch && context.purpose == EvaluationPurpose::Export)
            out->w = context.fullSourceW + 1;
          out->px.assign(static_cast<size_t>(out->w) * out->h, config.maskValue);
        } else {
          out->w = inputs.image->w; out->h = inputs.image->h;
          for (size_t i = 0; i < inputs.image->px.size(); i += 4) out->px.push_back(inputs.image->px[i]);
        }
        masks.push_back(out);
        result.outputs.emplace(ports::mask, MaskValue(out));
        if (node.operation == NodeOperation::TestMultipleOutputs) {
          auto doubled = std::make_shared<Image>(*inputs.image);
          for (size_t i = 0; i < doubled->px.size(); ++i) if (i % 4 != 3) doubled->px[i] *= 2.0f;
          images.push_back(doubled);
          result.outputs.emplace(ports::image, ImageValue(doubled));
          result.outputs.emplace(PortId{"original"}, inputs.image);
        }
        break;
      }
      case NodeOperation::TestMaskedImage: {
        auto out = std::make_shared<Image>(*inputs.image);
        for (size_t i = 0; i < out->px.size(); ++i)
          if (i % 4 != 3) out->px[i] += inputs.mask ? inputs.mask->px[i / 4] : 1.0f;
        images.push_back(out);
        result.outputs.emplace(ports::image, ImageValue(out));
        break;
      }
      case NodeOperation::TestSlotted: {
        if (inputs.slots.empty()) return {EvaluationState::Error, {}, "toy join has no input"};
        const auto frame = inputs.reference ? inputs.reference : inputs.slots.front().image;
        auto out = std::make_shared<Image>(*frame);
        for (size_t i = 0; i < out->px.size(); ++i) if (i % 4 != 3) out->px[i] = 0.0f;
        for (const auto &slot : inputs.slots) {
          slotOrder[node.id.value].push_back(slot.id);
          if (slot.image->w != frame->w || slot.image->h != frame->h)
            return {EvaluationState::Error, {}, "toy join Image dimensions differ"};
          for (size_t i = 0; i < out->px.size(); ++i)
            if (i % 4 != 3) {
              if (config.foldSlots) out->px[i] *= 10.0f;
              out->px[i] += slot.image->px[i] * (slot.mask ? slot.mask->px[i / 4] : 1.0f);
            }
        }
        images.push_back(out);
        result.outputs.emplace(ports::image, ImageValue(out));
        break;
      }
      case NodeOperation::TestUnavailableImage:
      case NodeOperation::TestUnavailableMask:
        return {EvaluationState::Unavailable, {}, "toy output unavailable"};
      default: return {EvaluationState::Error, {}, "unexpected toy operation"};
    }
    if (config.wrongType) result.outputs.begin()->second = std::monostate{};
    if (config.badShape) {
      if (auto value = std::get_if<MaskValue>(&result.outputs.begin()->second)) {
        auto invalid = std::make_shared<Mask>(**value); invalid->px.pop_back();
        *value = invalid;
      }
    }
    if (config.omitOutput) result.outputs.erase(result.outputs.begin());
    if (config.extraOutput) result.outputs.emplace(PortId{"undeclared"}, std::monostate{});
    return result;
  }
};

class RecordingProcessor final : public Processor {
 public:
  int renders = 0, sizes = 0, width = 0, height = 0, inputW = 0, inputH = 0;
  bool fail = false, throws = false, unknownException = false, sizeThrows = false, badRaster = false;
  bool interactive = false;
  std::function<void()> duringRender;
  ProcessorBackend backend() const override { return ProcessorBackend::Native; }
  std::string identifier() const override { return "test-recording"; }
  std::string displayName() const override { return "Test recording processor"; }
  std::vector<ProcessorParameter> parameters() const override { return {}; }
  bool setParameterValue(const std::string &, const ParameterValue &, bool) override { return false; }
  bool resetParameter(const std::string &, bool) override { return false; }
  bool activateParameter(const std::string &) override { return false; }
  void setRenderSize(int w, int h) override {
    ++sizes; width = w; height = h;
    if (sizeThrows) throw std::runtime_error("backend size exception");
  }
  ProcessorResult render(const Image &in, Image &out, const RenderCancellation &cancel) override {
    ++renders; inputW = in.w; inputH = in.h; interactive = cancel.interactive();
    if (duringRender) duringRender();
    if (throws) throw std::runtime_error("backend exception text");
    if (unknownException) throw 7;
    if (fail) return ProcessorResult::failure(23, "backend failure text");
    out = in;
    if (badRaster) out.px.pop_back();
    return ProcessorResult::success();
  }
};

static bool testSourceAndValidation() {
  Fixture f;
  const auto source = f.evaluate(image(f.source));
  CHECK(source.state == EvaluationState::Available && source.valueType == ValueType::Image);
  CHECK(source.requested == image(f.source) && pixels(source) == f.sourcePixels && source.diagnostics.empty());
  CHECK(f.output == image(f.source));
  const auto badNode = f.evaluate(image(NodeId{"absent"}));
  CHECK(badNode.state == EvaluationState::Error && !badNode.valueType && !pixels(badNode));
  CHECK(f.evaluate({f.source, ports::mask}).state == EvaluationState::Error);
  f.context.evalSourceW = 3;
  CHECK(message(f.evaluate(image(f.source)), "does not match"));
  f.context.evalSourceW = 0;
  CHECK(message(f.evaluate(image(f.source)), "Invalid evaluation"));
  f.context.evalSourceW = 2; f.sourcePixels.reset();
  CHECK(message(f.evaluate(image(f.source)), "Invalid Source"));
  f.sourcePixels = raster(2, 2);
  const NodeId a{"cycle-a"}, b{"cycle-b"};
  CHECK(f.add(a) && f.add(b));
  f.node(a)->fixedInputs.at(ports::image) = image(b);
  f.node(b)->fixedInputs.at(ports::image) = image(a);
  f.node(a)->enabled = false;
  CHECK(message(f.evaluate(image(f.source)), "cycle")); // Unreachable/bypassed stored cycles remain illegal.
  f.node(b)->fixedInputs.at(ports::image) = image(NodeId{"dangling"});
  CHECK(f.evaluate(image(a)).state == EvaluationState::Error);
  f.node(b)->fixedInputs.at(ports::image) = image(f.source);
  f.node(a)->fixedInputs.at(ports::image) = mask(b);
  CHECK(f.evaluate(image(a)).state == EvaluationState::Error);
  return true;
}

static bool testLinearStorageAndReachability() {
  Fixture f; ToyExecutor exec;
  const NodeId a{"gain-90"}, b{"offset-3"}, missing{"unused-missing"}, failed{"unused-failed"}, unavailable{"unused-unavailable"};
  CHECK(f.add(a) && f.add(b) && f.add(missing, NodeOperation::ImageProcessor) && f.add(failed) &&
        f.add(unavailable, NodeOperation::TestUnavailableMask));
  CHECK(f.connect(image(f.source), input(a)) && f.connect(image(a), input(b)));
  CHECK(f.connect(image(f.source), input(missing)) && f.connect(image(f.source), input(failed)));
  exec.settings[a.value].gain = 2.0f; exec.settings[b.value].add = 3.0f; exec.settings[failed.value].fail = true;
  auto result = f.evaluate(image(b), &exec);
  CHECK(result.state == EvaluationState::Available && pixels(result)->px[0] == 3.5f && result.diagnostics.empty());
  CHECK(exec.calls[a.value] == 1 && exec.calls[b.value] == 1 && exec.calls[failed.value] == 0 && exec.calls[unavailable.value] == 0);
  CHECK(pixels(result)->px[3] == f.sourcePixels->px[3]);
  std::reverse(f.nodes.begin(), f.nodes.end());
  const auto reordered = f.evaluate(image(b), &exec);
  CHECK(pixels(reordered)->px == pixels(result)->px && f.output == image(f.source));
  const auto intermediate = f.evaluate(image(a), &exec);
  CHECK(pixels(intermediate)->px[0] == 0.5f && exec.calls[b.value] == 2 && intermediate.diagnostics.empty());
  CHECK(exec.contexts[a.value].purpose == EvaluationPurpose::Inspection);
  return true;
}

static bool testFanoutBundleAndLifetime() {
  Fixture f; ToyExecutor exec;
  const NodeId shared{"shared"}, a{"branch-a"}, b{"branch-b"}, join{"join"}, bundle{"bundle"}, missing{"missing-shared"};
  CHECK(f.add(shared) && f.add(a) && f.add(b) && f.add(join, NodeOperation::TestSlotted) &&
        f.add(bundle, NodeOperation::TestMultipleOutputs) && f.add(missing, NodeOperation::ImageProcessor));
  CHECK(f.connect(image(f.source), input(missing)) && f.connect(image(missing), input(shared)) &&
        f.connect(image(shared), input(a)) && f.connect(image(shared), input(b)));
  exec.settings[a.value].add = 1.0f; exec.settings[b.value].add = 2.0f;
  const auto one = graph::addSlot(f.document(), join).addedSlot, two = graph::addSlot(f.document(), join).addedSlot;
  CHECK(one && two && f.connect(image(a), input(join, ports::image, one)) && f.connect(image(b), input(join, ports::image, two)));
  {
    const auto result = f.evaluate(image(join), &exec);
    CHECK(pixels(result)->px[0] == 3.5f && exec.calls[shared.value] == 1);
    CHECK(result.diagnostics.size() == 1 && result.diagnostics[0].endpoint == image(missing));
    CHECK(exec.images.size() == 4);
    CHECK(exec.images[0].expired() && exec.images[1].expired() && exec.images[2].expired() && !exec.images[3].expired());
  }
  CHECK(std::all_of(exec.images.begin(), exec.images.end(), [](const auto &p) { return p.expired(); }));
  CHECK(f.evaluate(image(join), &exec).state == EvaluationState::Available && exec.calls[shared.value] == 2);
  CHECK(f.connect(image(f.source), input(bundle)));
  CHECK(graph::reconnect(f.document(), image(bundle), input(join, ports::image, one)).ok());
  CHECK(graph::reconnect(f.document(), {bundle, PortId{"original"}}, input(join, ports::image, two)).ok());
  const auto combined = f.evaluate(image(join), &exec);
  CHECK(pixels(combined)->px[0] == 0.75f && exec.calls[bundle.value] == 1);
  const auto original = f.evaluate({bundle, PortId{"original"}}, &exec);
  CHECK(original.requested.outputPortId == PortId{"original"} && pixels(original) == f.sourcePixels);
  const auto m = f.evaluate(mask(bundle), &exec);
  CHECK(m.valueType == ValueType::Mask && all(coverage(m), 0.25f) && exec.calls[bundle.value] == 3);
  return true;
}

static bool testProcessorAndSerialEquivalence() {
  App app;
  CHECK(addNativeExposureNode(app) && addNativeExposureNode(app));
  CHECK(app.nodes[1].processor->setParameterValue("exposure", 1.0));
  CHECK(app.nodes[2].processor->setParameterValue("exposure", -0.5));
  const auto a = app.nodes[1].id, b = app.nodes[2].id;
  CHECK(graph::connect(app, image(app.primarySource), input(a)).ok());
  CHECK(graph::connect(app, image(a), input(b)).ok());
  const auto source = raster(2, 2);
  const EvaluationContext context{EvaluationPurpose::Export, 2, 2, 2, 2};
  Image serial;
  CHECK(renderChain(app, *source, serial).ok);
  auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, image(b), context, source);
  CHECK(result.state == EvaluationState::Available && pixels(result)->px == serial.px);
  CHECK(pixels(result)->px[1] < 0.0f && pixels(result)->px[2] > 1.0f && pixels(result)->px[3] == source->px[3]);
  app.nodes[1].enabled = false;
  CHECK(renderChain(app, *source, serial).ok);
  result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, image(b), context, source);
  CHECK(pixels(result)->px == serial.px && result.diagnostics.empty());
  app.nodes[1].enabled = true; app.nodes[1].processor.reset(); app.nodes[1].storedIdentifier = "missing-backend-id";
  CHECK(renderChain(app, *source, serial).ok);
  result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, image(b), context, source);
  CHECK(pixels(result)->px == serial.px && message(result, "missing-backend-id"));
  const auto missing = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, image(a), context, source);
  CHECK(pixels(missing) == source && missing.diagnostics.size() == 1);
  app.nodes[1].enabled = false;
  CHECK(evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, image(a), context, source).diagnostics.empty());
  return true;
}

static bool testMaskValidationAndBypass() {
  Fixture f; ToyExecutor exec;
  const NodeId producer{"coverage"}, invert{"invert"}, consumer{"effect"};
  CHECK(f.add(producer, NodeOperation::TestImageToMask) && f.add(invert, NodeOperation::TestMaskProcessor) &&
        f.add(consumer, NodeOperation::TestMaskedImage));
  auto samples = std::make_shared<Image>(*raster(5, 1));
  const float red[] = {-2.0f, 0.0f, 0.4f, 1.0f, 2.0f};
  for (int i = 0; i < 5; ++i) samples->px[i * 4] = red[i];
  f.sourcePixels = samples; f.context = {EvaluationPurpose::Inspection, 5, 1, 5, 1};
  CHECK(f.connect(image(f.source), input(producer)) && f.connect(mask(producer), input(invert, ports::mask)));
  CHECK(f.connect(image(f.source), input(consumer)) && f.connect(mask(invert), input(consumer, ports::mask)));
  auto result = f.evaluate(mask(producer), &exec);
  CHECK(coverage(result)->px == std::vector<float>({0.0f, 0.0f, 0.4f, 1.0f, 1.0f}));
  const auto inverted = f.evaluate(mask(invert), &exec);
  CHECK(coverage(inverted)->px == std::vector<float>({1.0f, 1.0f, 0.6f, 0.0f, 0.0f}));
  CHECK(pixels(f.evaluate(image(consumer), &exec))->px[0] == -1.0f);
  f.node(invert)->enabled = false;
  CHECK(coverage(f.evaluate(mask(invert), &exec))->px == coverage(result)->px);
  f.node(producer)->enabled = false;
  CHECK(all(coverage(f.evaluate(mask(producer), &exec)), 1.0f));
  f.node(producer)->enabled = true;
  exec.settings[producer.value].wrongType = true;
  CHECK(message(f.evaluate(mask(producer), &exec), "Invalid Mask"));
  exec.settings[producer.value].wrongType = false; exec.settings[producer.value].badShape = true;
  CHECK(f.evaluate(mask(producer), &exec).state == EvaluationState::Error);
  exec.settings[producer.value].badShape = false;
  samples->px[0] = std::numeric_limits<float>::quiet_NaN();
  CHECK(message(f.evaluate(mask(producer), &exec), "Non-finite"));
  samples->px[0] = 0.0f;
  exec.settings[producer.value].omitOutput = true;
  CHECK(message(f.evaluate(mask(producer), &exec), "all declared outputs"));
  exec.settings[producer.value].omitOutput = false; exec.settings[producer.value].extraOutput = true;
  CHECK(message(f.evaluate(mask(producer), &exec), "undeclared output"));
  return true;
}

static bool testBypassAwareDependencies() {
  Fixture f; ToyExecutor exec;
  const NodeId consumer{"bypassed-image"}, broken{"incomplete-mask"};
  CHECK(f.add(consumer, NodeOperation::TestMaskedImage) && f.add(broken, NodeOperation::TestMaskProcessor));
  CHECK(f.connect(image(f.source), input(consumer)) && f.connect(mask(broken), input(consumer, ports::mask)));
  f.node(consumer)->enabled = false;
  CHECK(!requestedGraphError(f.view(), image(consumer)).empty());
  auto result = f.evaluate(image(consumer), &exec);
  CHECK(result.state == EvaluationState::Available && pixels(result) == f.sourcePixels && result.diagnostics.empty());
  CHECK(exec.calls[broken.value] == 0 && exec.calls[consumer.value] == 0);
  f.node(consumer)->enabled = true;
  CHECK(message(f.evaluate(image(consumer), &exec), "Missing required input"));
  CHECK(exec.calls[consumer.value] == 0);
  CHECK(f.evaluate(image(consumer)).state == EvaluationState::Error);
  f.node(consumer)->enabled = false;
  f.node(broken)->fixedInputs.at(ports::mask) = mask(broken);
  CHECK(message(f.evaluate(image(consumer), &exec), "cycle"));
  return true;
}

static bool testUnavailableAndFailOpen() {
  Fixture f; ToyExecutor exec;
  const NodeId unavailable{"absent-mask"}, invert{"invert"}, a{"effect-a"}, b{"effect-b"}, join{"join"}, absentImage{"absent-image"};
  CHECK(f.add(unavailable, NodeOperation::TestUnavailableMask) && f.add(invert, NodeOperation::TestMaskProcessor) &&
        f.add(a, NodeOperation::TestMaskedImage) && f.add(b, NodeOperation::TestMaskedImage) &&
        f.add(join, NodeOperation::TestSlotted) && f.add(absentImage, NodeOperation::TestUnavailableImage));
  CHECK(f.connect(mask(unavailable), input(invert, ports::mask)));
  auto direct = f.evaluate(mask(unavailable), &exec);
  CHECK(direct.state == EvaluationState::Unavailable && direct.valueType == ValueType::Mask && !coverage(direct));
  auto inverted = f.evaluate(mask(invert), &exec);
  CHECK(inverted.state == EvaluationState::Unavailable && !coverage(inverted) && exec.calls[invert.value] == 0);
  CHECK(f.connect(image(f.source), input(a)) && f.connect(image(f.source), input(b)));
  CHECK(pixels(f.evaluate(image(a), &exec))->px[0] == 1.25f); // Absent optional Mask is full coverage.
  CHECK(f.connect(mask(invert), input(a, ports::mask)) && f.connect(mask(invert), input(b, ports::mask)));
  const auto one = graph::addSlot(f.document(), join).addedSlot, two = graph::addSlot(f.document(), join).addedSlot;
  CHECK(one && two && f.connect(image(a), input(join, ports::image, one)) && f.connect(image(b), input(join, ports::image, two)));
  const int before = exec.calls[unavailable.value];
  auto combined = f.evaluate(image(join), &exec);
  CHECK(combined.state == EvaluationState::Available && pixels(combined)->px[0] == 2.5f);
  CHECK(exec.calls[unavailable.value] == before + 1 && combined.diagnostics.size() == 3);
  CHECK(message(combined, "toy output unavailable") && message(combined, "using full coverage"));
  CHECK(graph::reconnect(f.document(), image(absentImage), input(a)).ok());
  const int aCalls = exec.calls[a.value];
  auto required = f.evaluate(image(a), &exec);
  CHECK(required.state == EvaluationState::Unavailable && !pixels(required) && exec.calls[a.value] == aCalls);
  return true;
}

static bool testReferenceAndGeneratorBypass() {
  Fixture f; ToyExecutor exec;
  const NodeId generator{"generator"}, invert{"invert"}, resized{"small-frame"}, unavailable{"unavailable-frame"}, effect{"effect"};
  CHECK(f.add(generator, NodeOperation::TestReferenceMask) && f.add(invert, NodeOperation::TestMaskProcessor) &&
        f.add(resized) && f.add(unavailable, NodeOperation::TestUnavailableImage) && f.add(effect, NodeOperation::TestMaskedImage));
  CHECK(f.connect(mask(generator), input(invert, ports::mask)) && f.connect(image(f.source), input(resized)));
  CHECK(f.connect(image(f.source), input(effect)) && f.connect(mask(invert), input(effect, ports::mask)));
  CHECK(all(coverage(f.evaluate(mask(generator), &exec)), 0.25f)); // Absent Reference uses this evaluation's Source frame.
  f.node(generator)->enabled = false;
  auto white = f.evaluate(mask(generator), &exec);
  CHECK(all(coverage(white), 1.0f) && coverage(white)->w == 2 && exec.calls[generator.value] == 1);
  CHECK(all(coverage(f.evaluate(mask(invert), &exec)), 0.0f));
  exec.settings[resized.value].halfSize = true;
  CHECK(f.connect(image(resized), input(generator, ports::reference)));
  white = f.evaluate(mask(generator), &exec);
  CHECK(all(coverage(white), 1.0f) && coverage(white)->w == 1 && coverage(white)->h == 1);
  CHECK(message(f.evaluate(image(effect), &exec), "dimension mismatch")); // White is real data, never fail-open.
  CHECK(graph::reconnect(f.document(), image(unavailable), input(generator, ports::reference)).ok());
  auto unavailableMask = f.evaluate(mask(generator), &exec);
  CHECK(unavailableMask.state == EvaluationState::Unavailable && !coverage(unavailableMask));
  CHECK(f.evaluate(mask(invert), &exec).state == EvaluationState::Unavailable);
  auto failOpen = f.evaluate(image(effect), &exec);
  CHECK(failOpen.state == EvaluationState::Available && pixels(failOpen)->px[0] == 1.25f && message(failOpen, "using full coverage"));
  f.node(generator)->enabled = true;
  CHECK(f.evaluate(mask(generator), &exec).state == EvaluationState::Unavailable && exec.calls[generator.value] == 1);
  return true;
}

static bool testDimensionsAndContext() {
  Fixture f; ToyExecutor exec;
  const NodeId resized{"size-change"}, processor{"actual-size-processor"}, generator{"context-mask"}, consumer{"consumer"};
  CHECK(f.add(resized) && f.add(processor, NodeOperation::ImageProcessor) &&
        f.add(generator, NodeOperation::TestReferenceMask) && f.add(consumer, NodeOperation::TestMaskedImage));
  auto recording = std::make_unique<RecordingProcessor>(); auto *record = recording.get();
  f.node(processor)->processor = std::move(recording);
  CHECK(f.connect(image(f.source), input(resized)) && f.connect(image(resized), input(processor)));
  exec.settings[resized.value].halfSize = true;
  f.sourcePixels = raster(100, 80); f.context = {EvaluationPurpose::Export, 100, 80, 100, 80};
  auto result = f.evaluate(image(processor), &exec);
  CHECK(result.state == EvaluationState::Available && pixels(result)->w == 50 && pixels(result)->h == 40);
  CHECK(record->sizes == 1 && record->width == 50 && record->height == 40 && record->inputW == 50 && record->inputH == 40);
  CHECK(pixels(result)->px[3] == 0.375f);
  CHECK(f.connect(image(f.source), input(consumer)) && f.connect(mask(generator), input(consumer, ports::mask)));
  exec.settings[generator.value].exportMismatch = true;
  f.sourcePixels = raster(2, 2); f.context = {EvaluationPurpose::Preview, 4, 4, 2, 2};
  CHECK(f.evaluate(image(consumer), &exec).state == EvaluationState::Available);
  CHECK(exec.contexts[generator.value].fullSourceW == 4 && exec.contexts[generator.value].evalSourceW == 2);
  f.sourcePixels = raster(4, 4); f.context = {EvaluationPurpose::Export, 4, 4, 4, 4};
  result = f.evaluate(image(consumer), &exec);
  CHECK(result.state == EvaluationState::Error && message(result, "Image 4x4, Mask 5x4"));
  CHECK(exec.calls[consumer.value] == 1 && exec.calls[generator.value] == 2);
  return true;
}

static bool testOfxOutputDimensions() {
  Fixture f;
  const NodeId crop{"ofx-crop"}, downstream{"after-crop"};
  const auto plugin = std::find_if(gPlugins.begin(), gPlugins.end(), [](const PluginEntry &entry) { return entry.label == "Crop"; });
  CHECK(plugin != gPlugins.end());
  CHECK(f.add(crop, NodeOperation::ImageProcessor) && f.add(downstream, NodeOperation::ImageProcessor));
  f.node(crop)->processor = OfxProcessor::create(static_cast<int>(std::distance(gPlugins.begin(), plugin)));
  CHECK(f.node(crop)->processor && f.node(crop)->processor->setParameterValue("crop", 40.0));
  auto recording = std::make_unique<RecordingProcessor>(); auto *record = recording.get();
  f.node(downstream)->processor = std::move(recording);
  CHECK(f.connect(image(f.source), input(crop)) && f.connect(image(crop), input(downstream)));
  f.sourcePixels = raster(100, 80); f.context = {EvaluationPurpose::Export, 100, 80, 100, 80};
  const auto result = f.evaluate(image(downstream));
  CHECK(result.state == EvaluationState::Available && pixels(result)->w < 100 && pixels(result)->h < 80);
  CHECK(record->width == pixels(result)->w && record->height == pixels(result)->h &&
        record->inputW == record->width && record->inputH == record->height);
  CHECK(pixels(result)->px[3] == 0.375f);
  return true;
}

static bool testSlots() {
  Fixture f; ToyExecutor exec;
  const NodeId join{"ordered-join"}, a{"one"}, b{"two"}, c{"three"}, broken{"unused-mask"};
  CHECK(f.add(join, NodeOperation::TestSlotted) && f.add(a) && f.add(b) && f.add(c) &&
        f.add(broken, NodeOperation::TestMaskProcessor));
  exec.settings[join.value].foldSlots = true;
  const NodeId layers[] = {a, b, c};
  std::vector<SlotId> ids;
  for (int i = 0; i < 3; ++i) {
    CHECK(f.connect(image(f.source), input(layers[i])));
    exec.settings[layers[i].value].gain = 0.0f; exec.settings[layers[i].value].add = static_cast<float>(i + 1);
    const auto slot = graph::addSlot(f.document(), join).addedSlot;
    CHECK(slot && f.connect(image(layers[i]), input(join, ports::image, slot)));
    ids.push_back(*slot);
  }
  CHECK(graph::reorderSlot(f.document(), join, ids[2], ids[0]).ok());
  const auto result = f.evaluate(image(join), &exec);
  CHECK(pixels(result)->px[0] == 312.0f && exec.slotOrder[join.value] == std::vector<SlotId>({ids[2], ids[0], ids[1]}));
  CHECK(result.diagnostics.empty()); // All optional Slot Masks and Reference are absent.
  CHECK(f.connect(mask(broken), input(join, ports::mask, ids[2])));
  CHECK(graph::disconnect(f.document(), input(join, ports::image, ids[1])).ok());
  f.node(join)->enabled = false;
  CHECK(pixels(f.evaluate(image(join), &exec))->px[0] == 3.0f && exec.calls[broken.value] == 0);
  f.node(join)->enabled = true;
  CHECK(message(f.evaluate(image(join), &exec), "Missing required input"));
  CHECK(graph::disconnect(f.document(), input(join, ports::mask, ids[2])).ok());
  const auto required = f.evaluate(image(join), &exec);
  CHECK(required.state == EvaluationState::Error && message(required, "[Slot 2].image"));
  f.node(join)->findSlot(ids[1])->settings.enabled = false;
  CHECK(f.evaluate(image(join), &exec).state == EvaluationState::Available);
  return true;
}

static bool testCancellationAndFailures() {
  Fixture f; ToyExecutor exec;
  const NodeId a{"counted-upstream"}, b{"suppressed-downstream"}, p{"backend"};
  CHECK(f.add(a) && f.add(b) && f.add(p, NodeOperation::ImageProcessor));
  CHECK(f.connect(image(f.source), input(a)) && f.connect(image(a), input(b)) && f.connect(image(f.source), input(p)));
  std::atomic<int> epoch{1}; const auto token = RenderCancellationTestAccess::token(epoch, 1);
  exec.settings[a.value].duringExecution = [&] { epoch.fetch_add(1); };
  auto result = f.evaluate(image(b), &exec, token);
  CHECK(result.state == EvaluationState::Cancelled && std::holds_alternative<std::monostate>(result.value));
  CHECK(exec.calls[a.value] == 1 && exec.calls[b.value] == 0 && result.diagnostics.empty());
  CHECK(f.evaluate(image(b), &exec, token).state == EvaluationState::Cancelled && exec.calls[a.value] == 1);
  exec.settings[a.value].duringExecution = {}; exec.settings[a.value].fail = true;
  CHECK(message(f.evaluate(image(b), &exec), "toy execution failure") && exec.calls[b.value] == 0);
  exec.settings[a.value].fail = false; exec.settings[a.value].throwException = true;
  CHECK(message(f.evaluate(image(b), &exec), "toy exception text") && exec.calls[b.value] == 0);
  auto recording = std::make_unique<RecordingProcessor>(); auto *record = recording.get();
  f.node(p)->processor = std::move(recording);
  CHECK(graph::disconnect(f.document(), input(p)).ok());
  CHECK(message(f.evaluate(image(p)), "Missing required input") && record->renders == 0 && record->sizes == 0);
  CHECK(f.connect(image(f.source), input(p)));
  record->fail = true;
  result = f.evaluate(image(p));
  CHECK(result.state == EvaluationState::Error && message(result, "23") && message(result, "backend failure text"));
  record->fail = false; record->throws = true;
  CHECK(message(f.evaluate(image(p)), "backend exception text"));
  record->throws = false; record->unknownException = true;
  CHECK(message(f.evaluate(image(p)), "unknown exception"));
  record->unknownException = false; record->sizeThrows = true;
  const int renders = record->renders;
  CHECK(message(f.evaluate(image(p)), "backend size exception") && record->renders == renders);
  record->sizeThrows = false; record->badRaster = true;
  CHECK(message(f.evaluate(image(p)), "Invalid Image"));
  record->badRaster = false; record->fail = true;
  epoch.store(1); record->duringRender = [&] { epoch.fetch_add(1); };
  result = f.evaluate(image(p), nullptr, token);
  CHECK(result.state == EvaluationState::Cancelled && result.diagnostics.empty() && record->interactive);
  return true;
}

}  // namespace

bool testGraphEvaluator() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"6C Source sharing, requested endpoints and malformed graph rejection", testSourceAndValidation},
      {"6C linear evaluation, storage independence and reachable-only diagnostics", testLinearStorageAndReachability},
      {"6C fan-out memoization, distinct output bundles and intermediate lifetime", testFanoutBundleAndLifetime},
      {"6C native Processor execution matches serial pixels, alpha and missing/bypass behaviour", testProcessorAndSerialEquivalence},
      {"6C scalar Mask coverage, clamping, Invert, bypass and malformed values", testMaskValidationAndBypass},
      {"6C bypass-aware readiness skips unused incomplete dependencies", testBypassAwareDependencies},
      {"6C Unavailable propagation and optional coverage fail-open warnings", testUnavailableAndFailOpen},
      {"6C Reference frames, generator white bypass and Unavailable distinction", testReferenceAndGeneratorBypass},
      {"6C actual intermediate dimensions and per-evaluation preview/export compatibility", testDimensionsAndContext},
      {"6C ordered Slot traversal, required inputs, optional coverage and bypass", testSlots},
      {"6C cooperative cancellation suppresses downstream work; backend failures remain errors", testCancellationAndFailures},
  };
  for (const auto &test : cases) {
    if (!test.test()) {
      std::fprintf(stderr, "selftest FAILED: %s\n", test.name);
      return false;
    }
    std::printf("ok  %s\n", test.name);
  }
  return true;
}

bool testGraphEvaluatorOfx() {
  if (!testOfxOutputDimensions()) return false;
  std::printf("ok  6C real OFX output dimensions reach downstream Processor setup/render\n");
  return true;
}
