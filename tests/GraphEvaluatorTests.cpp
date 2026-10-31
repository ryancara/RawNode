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
  std::vector<float> maskSamples;
  bool halfSize = false, foldSlots = false, exportMismatch = false;
  bool fail = false, throwException = false, wrongType = false, badShape = false;
  bool omitOutput = false, extraOutput = false, returnInactive = false;
  std::function<void()> duringExecution;
};
class ToyExecutor final : public graph_test::Executor {
 public:
  std::map<std::string, ToySettings> settings;
  std::map<std::string, int> calls;
  std::map<std::string, std::vector<SlotId>> slotOrder;
  std::map<std::string, EvaluationContext> contexts;
  std::vector<std::weak_ptr<const Image>> images, inputsSeen;
  std::vector<std::weak_ptr<const Mask>> masks;

  graph_test::OperationResult execute(const Node &node, const EvaluationContext &context,
                                     const graph_test::InputValues &inputs, const RenderCancellation &) override {
    ++calls[node.id.value];
    if (inputs.image) inputsSeen.push_back(inputs.image);
    contexts[node.id.value] = context;
    auto &config = settings[node.id.value];
    if (config.duringExecution) config.duringExecution();
    if (config.throwException) throw std::runtime_error("toy exception text");
    if (config.fail) return {EvaluationState::Error, {}, "toy execution failure"};
    if (config.returnInactive) return {EvaluationState::Inactive, {}, {}};
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
          out->px = config.maskSamples.empty() ? std::vector<float>(static_cast<size_t>(out->w) * out->h, config.maskValue) : config.maskSamples;
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
  bool interactive = false, halfSize = false;
  std::optional<CoordinateMapping> mapping;
  int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
  float gain = 1;
  const Image *lastOutput = nullptr;
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
    lastOutput = &out;
    if (halfSize) { out.w = std::max(1, in.w / 2); out.h = std::max(1, in.h / 2); out.px.resize(size_t(out.w) * out.h * 4); }
    if (cropW > 0 && cropH > 0) {
      out.w = cropW; out.h = cropH;
      out.px.resize(size_t(out.w) * out.h * 4);
      for (int y = 0; y < cropH; ++y)
        std::copy_n(in.px.begin() + (size_t(y + cropY) * in.w + cropX) * 4, size_t(cropW) * 4,
                    out.px.begin() + size_t(y) * cropW * 4);
    }
    for (size_t i = 0; i < out.px.size(); ++i) if (i % 4 != 3) out.px[i] *= gain;
    if (badRaster) out.px.pop_back();
    return ProcessorResult::success(mapping);
  }
};

// Tight double tolerance: these are short affine calculations in normalized
// coordinates, with analytical decimal/rational expectations (not image floats).
static bool mapsTo(const CoordinateMapping &mapping, double x, double y, double expectedX, double expectedY) {
  const auto mapped = mapping.map(x, y);
  return mapped && std::abs(mapped->first - expectedX) < 1e-12 && std::abs(mapped->second - expectedY) < 1e-12;
}

static bool testCoordinateMappingMath() {
  using Kind = CoordinateMapping::Kind;
  const auto identity = ProcessorResult::success().resolvedMapping(10, 8, 10, 8);
  CHECK(identity.kind() == Kind::Identity && mapsTo(identity, -.2, 1.3, -.2, 1.3));
  CHECK(ProcessorResult::success().resolvedMapping(10, 8, 5, 4).kind() == Kind::Unknown);
  const auto turn = CoordinateMapping::affine(0, -1, 1, 1, 0, .1);
  CHECK(ProcessorResult::success(turn).resolvedMapping(10, 8, 10, 8).kind() == Kind::Affine);
  CHECK(ProcessorResult::success(CoordinateMapping::unknown()).resolvedMapping(10, 8, 10, 8).kind() == Kind::Unknown);
  CHECK(ProcessorResult::success(identity).resolvedMapping(10, 8, 5, 4).kind() == Kind::Identity);
  // 10x8 cropped at pixel edge (2,1) to 5x4: x'=(10x-2)/5,
  // y'=(8y-1)/4. Then x''=1-y', y''=x'+.1. Order matters.
  const auto crop = CoordinateMapping::affine(2, 0, -.4, 0, 2, -.25);
  CHECK(mapsTo(crop, .35, .5, .3, .75));
  CHECK(mapsTo(crop.then(turn), .35, .5, .25, .4));
  CHECK(mapsTo(crop.then(turn).inverse(), .25, .4, .35, .5));
  CHECK(mapsTo(turn.then(crop), .35, .5, .6, .65));
  // Shear/off-diagonal terms, reflection, translation and a negative determinant.
  const auto shear = CoordinateMapping::affine(1, 2, .25, 3, -1, -.5);
  CHECK(mapsTo(shear, .2, .3, 1.05, -.2));
  CHECK(mapsTo(shear.inverse(), 1.05, -.2, .2, .3));
  CHECK(CoordinateMapping::affine(1, 2, 0, 2, 4, 0).inverse().kind() == Kind::Unknown);
  CHECK(CoordinateMapping::affine(1, 0, INFINITY, 0, 1, 0).kind() == Kind::Unknown);
  CHECK(!identity.map(NAN, 0) && !CoordinateMapping::unknown().map(0, 0));
  CHECK(crop.then(CoordinateMapping::unknown()).kind() == Kind::Unknown);
  CHECK(CoordinateMapping::unknown().then(crop).kind() == Kind::Unknown);
  CHECK(!CoordinateMapping::affine(1e308, 0, 0, 0, 1, 0).map(10, 0));
  CHECK(CoordinateMapping::affine(1e308, 0, 0, 0, 1, 0).then(crop).kind() == Kind::Unknown);
  return true;
}

static RecordingProcessor *recording(Fixture &f, const NodeId &id) {
  auto processor = std::make_unique<RecordingProcessor>();
  auto *record = processor.get();
  f.node(id)->processor = std::move(processor);
  return record;
}

static bool testCoordinatePlacementChain() {
  Fixture f; ToyExecutor exec;
  auto source = std::make_shared<Image>(*raster(10, 8));
  for (size_t i = 0; i < source->px.size(); ++i) source->px[i] = float(i);
  f.sourcePixels = source;
  f.context = {EvaluationPurpose::Inspection, 10, 8, 10, 8};
  const NodeId before{"before"}, effect{"effect"}, generator{"coverage"}, crop{"crop"}, turn{"turn"};
  for (const auto &id : {before, effect, crop, turn}) CHECK(f.add(id, NodeOperation::ImageProcessor));
  CHECK(f.add(generator, NodeOperation::TestReferenceMask));
  auto *pre = recording(f, before);
  pre->mapping = CoordinateMapping::affine(1, 0, .2, 0, 1, -.1);
  auto *adjustment = recording(f, effect);
  auto *cropping = recording(f, crop);
  auto *turning = recording(f, turn);
  CHECK(f.connect(image(f.source), input(before)) && f.connect(image(before), input(effect)) &&
        f.connect(mask(generator), input(effect, ports::mask)) && f.connect(image(effect), input(crop)) &&
        f.connect(image(crop), input(turn)));
  auto result = f.evaluate(image(crop), &exec);
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 1);
  CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Identity);
  CHECK(result.maskPlacements[0].mask == mask(generator) && result.maskPlacements[0].consumer.nodeId == effect);
  CHECK(result.maskPlacements[0].consumer.inputPortId == ports::mask && !result.maskPlacements[0].consumer.slotId);
  CHECK(pixels(result)->px == source->px); // Metadata did not change Mask mixing or photograph samples.
  cropping->cropX = 2; cropping->cropY = 1; cropping->cropW = 5; cropping->cropH = 4;
  result = f.evaluate(image(crop), &exec);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
  CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Unknown);
  // Real copied crop pixels, independently indexed into the original raster.
  CHECK(pixels(result)->w == 5 && pixels(result)->h == 4);
  CHECK(pixels(result)->px[0] == source->px[(1 * 10 + 2) * 4]);
  CHECK(pixels(result)->px[(3 * 5 + 4) * 4] == source->px[(4 * 10 + 6) * 4]);
  cropping->mapping = CoordinateMapping::affine(2, 0, -.4, 0, 2, -.25);
  turning->mapping = CoordinateMapping::affine(0, -1, 1, 1, 0, .1);
  // Both explicit Reference choices have the same raster shape but different
  // upstream geometry. Neither changes placement at the consuming adjustment.
  for (const auto &reference : {f.source, before}) {
    CHECK(graph::disconnect(f.document(), input(generator, ports::reference)).ok());
    CHECK(f.connect(image(reference), input(generator, ports::reference)));
    result = f.evaluate(image(turn), &exec);
    CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 1);
    const auto mapping = result.maskPlacements[0].maskToImage;
    CHECK(mapsTo(mapping, .35, .5, .25, .4));
    CHECK(mapsTo(mapping.inverse(), .25, .4, .35, .5));
  }
  // An earlier result owns its own mapping, independent of subsequent renders.
  const auto retained = result;
  cropping->mapping = CoordinateMapping::unknown();
  result = f.evaluate(image(turn), &exec);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
  CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Unknown);
  CHECK(mapsTo(retained.maskPlacements[0].maskToImage, .35, .5, .25, .4));
  // Bypass, exact zero and missing processor preserve upstream placement and
  // do not consume their own connected coverage (or validate its dimensions).
  CHECK(f.connect(mask(generator), input(crop, ports::mask)));
  for (int pass = 0; pass < 3; ++pass) {
    f.node(crop)->enabled = pass != 0;
    f.node(crop)->strength = pass == 1 ? 0 : 1;
    if (pass == 2) f.node(crop)->processor.reset();
    result = f.evaluate(image(crop), &exec);
    CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 1);
    CHECK(result.maskPlacements[0].consumer.nodeId == effect);
    CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Identity);
    CHECK(pixels(result)->w == 10 && pixels(result)->h == 8);
  }
  f.node(effect)->enabled = false;
  CHECK(f.evaluate(image(effect), &exec).maskPlacements.empty());
  f.node(effect)->enabled = true; f.node(effect)->strength = 0;
  CHECK(f.evaluate(image(effect), &exec).maskPlacements.empty());
  f.node(effect)->strength = 1; f.node(generator)->enabled = false;
  CHECK(f.evaluate(image(effect), &exec).maskPlacements.empty());
  f.node(generator)->enabled = true;
  CHECK(f.add(NodeId{"unavailable"}, NodeOperation::TestUnavailableMask));
  CHECK(graph::reconnect(f.document(), mask(NodeId{"unavailable"}), input(effect, ports::mask)).ok());
  result = f.evaluate(image(effect), &exec);
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.empty());
  CHECK(message(result, "full coverage") && adjustment->renders > 0);
  return true;
}

static bool testSharedCoordinatePlacementsAndMixing() {
  Fixture f; ToyExecutor exec;
  const NodeId a{"a"}, b{"b"}, move{"move"}, generator{"shared"};
  for (const auto &id : {a, b, move}) CHECK(f.add(id, NodeOperation::ImageProcessor));
  CHECK(f.add(generator, NodeOperation::TestReferenceMask));
  recording(f, a); recording(f, b); auto *transform = recording(f, move);
  transform->mapping = CoordinateMapping::affine(1, 0, .1, 0, 1, .2);
  CHECK(f.connect(image(f.source), input(a)) && f.connect(mask(generator), input(a, ports::mask)) &&
        f.connect(image(a), input(move)) && f.connect(image(move), input(b)) &&
        f.connect(mask(generator), input(b, ports::mask)));
  auto result = f.evaluate(image(b), &exec);
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 2);
  CHECK(exec.calls[generator.value] == 1);
  CHECK(result.maskPlacements[0].mask == result.maskPlacements[1].mask);
  CHECK(result.maskPlacements[0].consumer.nodeId == a && result.maskPlacements[1].consumer.nodeId == b);
  CHECK(mapsTo(result.maskPlacements[0].maskToImage, .2, .3, .3, .5));
  CHECK(mapsTo(result.maskPlacements[1].maskToImage, .2, .3, .2, .3));
  // Partial Strength with moved pixels cannot supply one upstream geometry.
  f.node(move)->strength = .5;
  result = f.evaluate(image(b), &exec);
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 2);
  CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Unknown);
  CHECK(result.maskPlacements[1].maskToImage.kind() == CoordinateMapping::Kind::Identity);
  f.node(move)->strength = 1;
  CHECK(f.connect(mask(generator), input(move, ports::mask)));
  result = f.evaluate(image(move), &exec);
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 2);
  CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Unknown);
  CHECK(result.maskPlacements[1].consumer.nodeId == move);
  CHECK(result.maskPlacements[1].maskToImage.kind() == CoordinateMapping::Kind::Identity);
  transform->mapping = CoordinateMapping::identity();
  result = f.evaluate(image(move), &exec);
  CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Identity);
  // Two roots retain their own correspondence even when evaluated in one memo.
  const auto pair = evaluateGraphPair(f.view(), image(a), image(b), f.context, f.sourcePixels, {}, &exec);
  CHECK(pair.first.maskPlacements.size() == 1 && pair.second.maskPlacements.size() == 3);
  CHECK(pair.first.maskPlacements[0].consumer.nodeId == a);
  // Divergent branches merge without choosing one consumer's placement. Even
  // the same Mask on two ordered Slots retains the consuming Slot identities.
  const NodeId join{"coordinate-join"};
  CHECK(f.add(join, NodeOperation::TestSlotted));
  const auto first = graph::addSlot(f.document(), join).addedSlot;
  const auto second = graph::addSlot(f.document(), join).addedSlot;
  CHECK(first && second && graph::reconnect(f.document(), image(f.source), input(b)).ok());
  CHECK(f.connect(image(a), input(join, ports::image, first)) &&
        f.connect(image(b), input(join, ports::image, second)) &&
        f.connect(mask(generator), input(join, ports::mask, first)) &&
        f.connect(mask(generator), input(join, ports::mask, second)));
  result = f.evaluate(image(join), &exec);
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 4);
  for (const auto &placement : result.maskPlacements) {
    CHECK(placement.mask == mask(generator));
    CHECK(placement.maskToImage.kind() == CoordinateMapping::Kind::Unknown);
  }
  CHECK(result.maskPlacements[1].consumer.slotId == first && result.maskPlacements[3].consumer.slotId == second);
  f.node(join)->enabled = false;
  result = f.evaluate(image(join), &exec);
  CHECK(result.state == EvaluationState::Available && result.maskPlacements.size() == 1);
  CHECK(result.maskPlacements[0].consumer.nodeId == a);
  CHECK(result.maskPlacements[0].maskToImage.kind() == CoordinateMapping::Kind::Identity);
  // Existing hard raster rules still apply; coordinate metadata cannot relax them.
  transform->halfSize = true;
  CHECK(f.evaluate(image(move), &exec).state == EvaluationState::Error);
  CHECK(f.evaluate(mask(generator), &exec).maskPlacements.empty());
  return true;
}

static bool testProductionMaskAndStrength() {
  Fixture f; ToyExecutor exec;
  const NodeId effect{"production"}, generator{"coverage"}, unavailable{"unavailable"};
  CHECK(f.add(effect, NodeOperation::ImageProcessor) && f.add(generator, NodeOperation::TestReferenceMask) &&
        f.add(unavailable, NodeOperation::TestUnavailableMask));
  auto backend = std::make_unique<RecordingProcessor>(); auto *record = backend.get(); record->gain = 4;
  f.node(effect)->processor = std::move(backend);
  CHECK(f.connect(image(f.source), input(effect)));
  for (int kind = 0; kind < 6; ++kind) {
    CHECK(graph::disconnect(f.document(), input(effect, ports::mask)).ok());
    f.node(generator)->enabled = kind != 1;
    exec.settings[generator.value].maskSamples.clear();
    exec.settings[generator.value].maskValue = kind == 3 ? 0 : 1;
    if (kind == 5) exec.settings[generator.value].maskSamples = {0, .25f, .5f, 1};
    if (kind) CHECK(f.connect(kind == 2 ? mask(unavailable) : mask(generator), input(effect, ports::mask)));
    for (double strength : {0.0, .5, 1.0}) {
      f.node(effect)->strength = strength;
      const int renders = record->renders, generates = exec.calls[generator.value], missing = exec.calls[unavailable.value];
      auto result = f.evaluate(image(effect), &exec);
      CHECK(result.state == EvaluationState::Available && result.valueType == ValueType::Image);
      CHECK(record->renders == renders + (strength != 0));
      CHECK(result.diagnostics.empty() == !(kind == 2 && strength != 0));
      if (strength == 0) {
        CHECK(pixels(result) == f.sourcePixels && exec.calls[generator.value] == generates && exec.calls[unavailable.value] == missing);
      } else if (strength == 1 && kind <= 2) CHECK(pixels(result).get() == record->lastOutput);
      for (size_t pixel = 0; pixel < 4; ++pixel) {
        const double coverage = kind == 3 ? 0 : kind == 5 ? exec.settings[generator.value].maskSamples[pixel] : 1;
        for (size_t c = 0; c < 4; ++c) {
          const float base = f.sourcePixels->px[pixel * 4 + c];
          const float expected = c == 3 ? base : float(base * (1 + 3 * strength * coverage));
          CHECK(pixels(result)->px[pixel * 4 + c] == expected);
        }
      }
      f.context.purpose = EvaluationPurpose::Export;
      const auto exported = f.evaluate(image(effect), &exec);
      CHECK(exported.state == EvaluationState::Available && pixels(exported)->px == pixels(result)->px);
      f.context.purpose = EvaluationPurpose::Preview;
    }
  }
  // Opposite infinities distinguish exact endpoint copies from interpolation.
  // Partial Strength must also leave completely unmasked pixels untouched.
  auto endpointSource = std::make_shared<Image>(*f.sourcePixels);
  const float infinity = std::numeric_limits<float>::infinity();
  endpointSource->px[0] = endpointSource->px[4] = infinity;
  f.sourcePixels = endpointSource;
  record->gain = -infinity;
  exec.settings[generator.value].maskSamples = {0, 1, .5f, 0};
  for (double strength : {.5, 1.0}) {
    f.node(effect)->strength = strength;
    const auto result = f.evaluate(image(effect), &exec);
    CHECK(result.state == EvaluationState::Available);
    const auto &samples = pixels(result)->px;
    CHECK(std::equal(samples.begin(), samples.begin() + 4, endpointSource->px.begin()));
    CHECK(std::equal(samples.begin() + 12, samples.end(), endpointSource->px.begin() + 12));
    if (strength == 1) {
      const std::vector<float> processed{-infinity, infinity, -infinity, .375f};
      CHECK(std::equal(processed.begin(), processed.end(), samples.begin() + 4));
    }
  }
  f.sourcePixels = raster(2, 2);
  record->gain = 4;
  // An active Mask is still a mixing request, even when all samples are white.
  record->halfSize = true;
  CHECK(graph::disconnect(f.document(), input(effect, ports::mask)).ok());
  f.node(effect)->strength = 1;
  CHECK(pixels(f.evaluate(image(effect), &exec))->w == 1);
  f.node(effect)->strength = 0;
  CHECK(pixels(f.evaluate(image(effect), &exec)) == f.sourcePixels);
  f.node(effect)->strength = .5;
  CHECK(message(f.evaluate(image(effect), &exec), "dimension mismatch"));
  f.node(effect)->strength = 1;
  exec.settings[generator.value].maskSamples.clear(); exec.settings[generator.value].maskValue = 1;
  CHECK(f.connect(mask(generator), input(effect, ports::mask)));
  CHECK(message(f.evaluate(image(effect), &exec), "dimension mismatch"));
  f.node(generator)->enabled = false;
  CHECK(pixels(f.evaluate(image(effect), &exec))->w == 1);
  f.node(generator)->enabled = true;
  record->halfSize = false;
  record->badRaster = true;
  CHECK(message(f.evaluate(image(effect), &exec), "Invalid Image"));
  record->badRaster = false;
  record->fail = true;
  CHECK(f.evaluate(image(effect), &exec).state == EvaluationState::Error);
  f.node(effect)->strength = 0;
  CHECK(pixels(f.evaluate(image(effect), &exec)) == f.sourcePixels);
  f.node(effect)->strength = .5;
  f.node(effect)->enabled = false;
  CHECK(pixels(f.evaluate(image(effect), &exec)) == f.sourcePixels);
  f.node(effect)->enabled = true;
  f.node(effect)->processor.reset();
  f.node(effect)->storedIdentifier = "missing-production";
  const auto missing = f.evaluate(image(effect), &exec);
  CHECK(pixels(missing) == f.sourcePixels && message(missing, "missing-production"));
  f.node(effect)->strength = 0;
  CHECK(f.evaluate(image(effect), &exec).diagnostics.empty());
  return true;
}

static bool testPlannedStrength() {
  Fixture f; ToyExecutor exec;
  const NodeId upstream{"upstream"}, effect{"effect"}, generator{"coverage"}, join{"join"};
  CHECK(f.add(upstream) && f.add(effect, NodeOperation::ImageProcessor) &&
        f.add(generator, NodeOperation::TestReferenceMask) && f.add(join, NodeOperation::TestSlotted));
  auto processor = std::make_unique<RecordingProcessor>(); auto *record = processor.get(); record->gain = 4;
  f.node(effect)->processor = std::move(processor);
  CHECK(f.connect(image(f.source), input(upstream)) && f.connect(image(upstream), input(effect)));
  // Change the live control after planning but before the effect's mix.
  exec.settings[upstream.value].duringExecution = [&] { f.node(effect)->strength = 0; };
  const auto processed = f.evaluate(image(effect), &exec);
  CHECK(processed.state == EvaluationState::Available && pixels(processed)->px[0] == 1 && record->renders == 1);
  CHECK(f.node(effect)->strength == 0);

  CHECK(graph::reconnect(f.document(), image(f.source), input(effect)).ok() &&
        f.connect(mask(generator), input(effect, ports::mask)));
  const auto a = graph::addSlot(f.document(), join).addedSlot, b = graph::addSlot(f.document(), join).addedSlot;
  CHECK(a && b && f.connect(image(upstream), {join, a, ports::image}) && f.connect(image(effect), {join, b, ports::image}));
  exec.settings[upstream.value].duringExecution = [&] { f.node(effect)->strength = 1; };
  exec.settings[generator.value].fail = true;
  // An earlier branch changes zero to one before this effect executes. Its
  // planned zero must still skip both processor and connected Mask dependency.
  const auto result = f.evaluate(image(join), &exec);
  CHECK(result.state == EvaluationState::Available && pixels(result)->px[0] == .5f);
  CHECK(record->renders == 1 && exec.calls[generator.value] == 0 && result.diagnostics.empty());
  return true;
}

static bool testProductionMixLifetime() {
  Fixture f; ToyExecutor exec;
  const NodeId shared{"shared-production-mix"}, a{"a"}, b{"b"}, join{"join"};
  CHECK(f.add(shared, NodeOperation::ImageProcessor) && f.add(a) && f.add(b) && f.add(join, NodeOperation::TestSlotted));
  auto processor = std::make_unique<RecordingProcessor>(); auto *record = processor.get(); record->gain = 2;
  f.node(shared)->processor = std::move(processor); f.node(shared)->strength = .5;
  CHECK(f.connect(image(f.source), input(shared)) && f.connect(image(shared), input(a)) && f.connect(image(shared), input(b)));
  const auto one = graph::addSlot(f.document(), join).addedSlot, two = graph::addSlot(f.document(), join).addedSlot;
  CHECK(one && two && f.connect(image(a), input(join, ports::image, one)) && f.connect(image(b), input(join, ports::image, two)));
  bool aliveAtBoth = true, releasedAtJoin = false;
  exec.settings[a.value].duringExecution = [&] { aliveAtBoth &= !exec.inputsSeen.front().expired(); };
  exec.settings[b.value].duringExecution = [&] { aliveAtBoth &= !exec.inputsSeen.front().expired(); };
  exec.settings[join.value].duringExecution = [&] { releasedAtJoin = exec.inputsSeen.front().expired(); };
  const auto result = f.evaluate(image(join), &exec);
  CHECK(result.state == EvaluationState::Available && pixels(result)->px[0] == .75f);
  CHECK(record->renders == 1 && aliveAtBoth && releasedAtJoin);
  return true;
}

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
  bool firstAlive = false, lastAlive = false, releasedBeforeJoin = false;
  exec.settings[a.value].duringExecution = [&] { firstAlive = !exec.images[0].expired(); };
  exec.settings[b.value].duringExecution = [&] { lastAlive = !exec.images[0].expired(); };
  exec.settings[join.value].duringExecution = [&] {
    releasedBeforeJoin = exec.images[0].expired() && !exec.images[1].expired() && !exec.images[2].expired();
  };
  {
    const auto result = f.evaluate(image(join), &exec);
    CHECK(pixels(result)->px[0] == 3.5f && exec.calls[shared.value] == 1);
    CHECK(result.diagnostics.size() == 1 && result.diagnostics[0].endpoint == image(missing));
    CHECK(exec.images.size() == 4);
    CHECK(firstAlive && lastAlive && releasedBeforeJoin);
    CHECK(exec.images[0].expired() && exec.images[1].expired() && exec.images[2].expired() && !exec.images[3].expired());
  }
  for (const auto &id : {a, b, join}) exec.settings[id.value].duringExecution = {};
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

static bool testLongChainRelease() {
  Fixture f; ToyExecutor exec;
  OutputRef previous = image(f.source);
  NodeId last;
  for (int i = 0; i < 40; ++i) {
    last = NodeId{"linear-" + std::to_string(i)};
    CHECK(f.add(last) && f.connect(previous, input(last)));
    previous = image(last);
  }
  bool releasedAtLastExecution = false;
  exec.settings[last.value].duringExecution = [&] {
    releasedAtLastExecution = exec.images.size() == 39 && !exec.images.back().expired() &&
        std::all_of(exec.images.begin(), exec.images.end() - 1, [](const auto &image) { return image.expired(); });
  };
  const auto result = f.evaluate(previous, &exec);
  CHECK(result.state == EvaluationState::Available && releasedAtLastExecution);
  CHECK(pixels(result)->px == f.sourcePixels->px);

  Fixture bundle; ToyExecutor bundled;
  const NodeId upstream{"owned-input"}, multi{"outputs"}, join{"joined-outputs"};
  CHECK(bundle.add(upstream) && bundle.add(multi, NodeOperation::TestMultipleOutputs) &&
        bundle.add(join, NodeOperation::TestSlotted));
  CHECK(bundle.connect(image(bundle.source), input(upstream)) && bundle.connect(image(upstream), input(multi)));
  const auto first = graph::addSlot(bundle.document(), join).addedSlot;
  const auto second = graph::addSlot(bundle.document(), join).addedSlot;
  CHECK(first && second && bundle.connect(image(multi), input(join, ports::image, first)) &&
        bundle.connect({multi, PortId{"original"}}, input(join, ports::image, second)));
  bool bundleAlive = false;
  bundled.settings[join.value].duringExecution = [&] {
    bundleAlive = !bundled.images[0].expired() && !bundled.images[1].expired() && bundled.masks[0].expired();
  };
  CHECK(bundle.evaluate(image(join), &bundled).state == EvaluationState::Available && bundleAlive && bundled.calls[multi.value] == 1);
  return true;
}

// Historical linear reference belongs only in this pixel test, never production.
static ProcessorResult serialReference(App &app, const Image &source, Image &out) {
  out = source;
  for (const auto &node : app.nodes) {
    if (node.operation != NodeOperation::ImageProcessor || !node.enabled || !node.processor) continue;
    Image next;
    const auto result = node.processor->render(out, next, {});
    if (!result.ok) return result;
    out = std::move(next);
  }
  return ProcessorResult::success();
}

static bool testProcessorAndSerialEquivalence() {
  App app;
  CHECK(addNativeExposureNode(app) && addNativeExposureNode(app));
  CHECK(app.nodes[1].processor->setParameterValue("exposure", 1.0));
  CHECK(app.nodes[2].processor->setParameterValue("exposure", -0.5));
  const auto a = app.nodes[1].id, b = app.nodes[2].id;

  const auto source = raster(2, 2);
  const EvaluationContext context{EvaluationPurpose::Export, 2, 2, 2, 2};
  Image serial;
  CHECK(serialReference(app, *source, serial).ok);
  auto result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, image(b), context, source);
  CHECK(result.state == EvaluationState::Available && pixels(result)->px == serial.px);
  CHECK(pixels(result)->px[1] < 0.0f && pixels(result)->px[2] > 1.0f && pixels(result)->px[3] == source->px[3]);
  app.nodes[1].enabled = false;
  CHECK(serialReference(app, *source, serial).ok);
  result = evaluateGraph({app.nodes, app.primarySource, app.documentOutput}, image(b), context, source);
  CHECK(pixels(result)->px == serial.px && result.diagnostics.empty());
  app.nodes[1].enabled = true; app.nodes[1].processor.reset(); app.nodes[1].storedIdentifier = "missing-backend-id";
  CHECK(serialReference(app, *source, serial).ok);
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
  const auto inactive = f.evaluate(mask(producer), &exec);
  CHECK(inactive.state == EvaluationState::Inactive && inactive.valueType == ValueType::Mask &&
        std::holds_alternative<std::monostate>(inactive.value) && inactive.diagnostics.empty());
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
  CHECK(all(coverage(f.evaluate(mask(generator), &exec)), 0.25f)); // Absent Reference uses the evaluation's Source frame.
  f.node(generator)->enabled = false;
  auto inactive = f.evaluate(mask(generator), &exec);
  CHECK(inactive.state == EvaluationState::Inactive && inactive.valueType == ValueType::Mask &&
        std::holds_alternative<std::monostate>(inactive.value) && inactive.diagnostics.empty());
  CHECK(exec.calls[generator.value] == 1);
  CHECK(f.evaluate(mask(invert), &exec).state == EvaluationState::Inactive && exec.calls[invert.value] == 0);
  exec.settings[resized.value].halfSize = true;
  CHECK(f.connect(image(resized), input(generator, ports::reference)));
  CHECK(f.evaluate(mask(generator), &exec).state == EvaluationState::Inactive && exec.calls[resized.value] == 0);
  auto unrestricted = f.evaluate(image(effect), &exec);
  CHECK(unrestricted.state == EvaluationState::Available && pixels(unrestricted)->px[0] == 1.25f &&
        unrestricted.diagnostics.empty() && exec.calls[resized.value] == 0);
  // Once enabled, a real wrong-sized Mask still fails rather than becoming inactive.
  f.node(generator)->enabled = true;
  CHECK(message(f.evaluate(image(effect), &exec), "dimension mismatch"));
  CHECK(exec.calls[resized.value] == 1 && exec.calls[generator.value] == 2);
  CHECK(graph::reconnect(f.document(), image(unavailable), input(generator, ports::reference)).ok());
  f.node(generator)->enabled = false;
  CHECK(f.evaluate(mask(generator), &exec).state == EvaluationState::Inactive && exec.calls[unavailable.value] == 0);
  unrestricted = f.evaluate(image(effect), &exec);
  CHECK(pixels(unrestricted)->px[0] == 1.25f && unrestricted.diagnostics.empty());
  f.node(generator)->enabled = true;
  auto unavailableMask = f.evaluate(mask(generator), &exec);
  CHECK(unavailableMask.state == EvaluationState::Unavailable && !coverage(unavailableMask));
  CHECK(f.evaluate(mask(invert), &exec).state == EvaluationState::Unavailable);
  auto failOpen = f.evaluate(image(effect), &exec);
  CHECK(failOpen.state == EvaluationState::Available && pixels(failOpen)->px[0] == 1.25f && message(failOpen, "using full coverage"));
  CHECK(exec.calls[generator.value] == 2 && exec.calls[invert.value] == 1);
  return true;
}

static bool testInactivePropagationAndFanout() {
  Fixture f; ToyExecutor exec;
  const NodeId generator{"generator"}, modifier{"modifier"}, a{"effect-a"}, b{"effect-b"}, join{"join"};
  CHECK(f.add(generator, NodeOperation::TestReferenceMask) && f.add(modifier, NodeOperation::TestMaskProcessor) &&
        f.add(a, NodeOperation::TestMaskedImage) && f.add(b, NodeOperation::TestMaskedImage) &&
        f.add(join, NodeOperation::TestSlotted));
  CHECK(f.connect(mask(generator), input(modifier, ports::mask)) &&
        f.connect(image(f.source), input(a)) && f.connect(image(f.source), input(b)) &&
        f.connect(mask(modifier), input(a, ports::mask)) && f.connect(mask(modifier), input(b, ports::mask)));
  const auto one = graph::addSlot(f.document(), join).addedSlot, two = graph::addSlot(f.document(), join).addedSlot;
  CHECK(one && two && f.connect(image(a), input(join, ports::image, one)) &&
        f.connect(image(b), input(join, ports::image, two)) && f.connect(mask(modifier), input(join, ports::mask, one)));
  f.node(generator)->enabled = false;
  for (bool enabled : {true, false}) {
    f.node(modifier)->enabled = enabled;
    const auto inactive = f.evaluate(mask(modifier), &exec);
    CHECK(inactive.state == EvaluationState::Inactive && inactive.valueType == ValueType::Mask &&
          std::holds_alternative<std::monostate>(inactive.value) && inactive.diagnostics.empty());
    const auto result = f.evaluate(image(join), &exec);
    CHECK(result.state == EvaluationState::Available && pixels(result)->px[0] == 2.5f && result.diagnostics.empty());
    CHECK(exec.calls[generator.value] == 0 && exec.calls[modifier.value] == 0);
  }
  // Real constant Masks remain data, including through Invert and optional coverage.
  f.node(generator)->enabled = true;
  f.node(modifier)->enabled = true;
  for (float sample : {0.0f, 1.0f}) {
    exec.settings[generator.value].maskValue = sample;
    CHECK(all(coverage(f.evaluate(mask(generator), &exec)), sample));
    CHECK(all(coverage(f.evaluate(mask(modifier), &exec)), 1.0f - sample));
    const int calls = exec.calls[generator.value];
    const auto result = f.evaluate(image(join), &exec);
    CHECK(pixels(result)->px[0] == (sample == 0.0f ? 2.5f : 0.25f));
    CHECK(exec.calls[generator.value] == calls + 1); // Shared producer still executes once.
    CHECK(result.diagnostics.empty());
  }
  CHECK(graph::disconnect(f.document(), input(modifier, ports::mask)).ok());
  CHECK(message(f.evaluate(mask(modifier), &exec), "Missing required input"));
  return true;
}

static bool testPerOutputBypassAndMissingData() {
  Fixture f; ToyExecutor exec;
  const NodeId analysis{"analysis"}, missingImage{"missing-image"}, missingMask{"missing-mask"},
      modifier{"modifier"}, bundle{"bundle"}, effect{"effect"}, join{"join"};
  CHECK(f.add(analysis, NodeOperation::TestImageToMask) && f.add(missingImage, NodeOperation::ImageProcessor) &&
        f.add(missingMask, NodeOperation::TestUnavailableMask) && f.add(modifier, NodeOperation::TestMaskProcessor) &&
        f.add(bundle, NodeOperation::TestMultipleOutputs) && f.add(effect, NodeOperation::TestMaskedImage) &&
        f.add(join, NodeOperation::TestSlotted));
  CHECK(f.connect(image(f.source), input(missingImage)) && f.connect(image(missingImage), input(analysis)) &&
        f.connect(mask(missingMask), input(modifier, ports::mask)) && f.connect(image(missingImage), input(bundle)) &&
        f.connect(image(bundle), input(effect)) && f.connect(mask(bundle), input(effect, ports::mask)));
  f.node(missingImage)->storedBackend = "ctl";
  f.node(missingImage)->storedIdentifier = "missing-analysis.ctl";
  CHECK(message(f.evaluate(mask(analysis), &exec), "missing-analysis.ctl"));
  f.node(analysis)->enabled = false;
  const auto inactive = f.evaluate(mask(analysis)); // No executor needed for bypass.
  CHECK(inactive.state == EvaluationState::Inactive && inactive.diagnostics.empty());
  CHECK(graph::disconnect(f.document(), input(analysis)).ok());
  CHECK(f.evaluate(mask(analysis)).state == EvaluationState::Inactive);
  f.node(analysis)->enabled = true;
  CHECK(message(f.evaluate(mask(analysis), &exec), "Missing required input"));

  // Missing Mask backend is represented by the existing unavailable test producer.
  f.node(missingMask)->storedIdentifier = "missing-mask-backend";
  CHECK(f.evaluate(mask(modifier), &exec).state == EvaluationState::Unavailable);
  f.node(modifier)->enabled = false;
  CHECK(f.evaluate(mask(modifier), &exec).state == EvaluationState::Unavailable);
  f.node(missingMask)->enabled = false;
  const auto bypassedMissing = f.evaluate(mask(modifier));
  CHECK(bypassedMissing.state == EvaluationState::Inactive && bypassedMissing.diagnostics.empty() &&
        std::holds_alternative<std::monostate>(bypassedMissing.value));
  CHECK(f.node(missingMask)->storedIdentifier == "missing-mask-backend");
  CHECK(f.node(missingImage)->isMissingProcessor() && f.node(missingImage)->storedIdentifier == "missing-analysis.ctl");

  f.node(bundle)->enabled = false;
  CHECK(f.evaluate(mask(bundle)).state == EvaluationState::Inactive);
  for (const auto &endpoint : {image(bundle), OutputRef{bundle, PortId{"original"}}}) {
    const auto passed = f.evaluate(endpoint);
    CHECK(passed.state == EvaluationState::Available && pixels(passed) == f.sourcePixels &&
          message(passed, "missing-analysis.ctl"));
  }
  const auto one = graph::addSlot(f.document(), join).addedSlot, two = graph::addSlot(f.document(), join).addedSlot;
  CHECK(one && two && f.connect(image(effect), input(join, ports::image, one)) &&
        f.connect({bundle, PortId{"original"}}, input(join, ports::image, two)));
  const auto result = f.evaluate(image(join), &exec);
  CHECK(result.state == EvaluationState::Available && pixels(result)->px[0] == 1.5f && result.diagnostics.size() == 1);
  CHECK(exec.calls[bundle.value] == 0);
  CHECK(graph::disconnect(f.document(), input(bundle)).ok());
  CHECK(f.evaluate(mask(bundle)).state == EvaluationState::Inactive);
  CHECK(message(f.evaluate(image(bundle)), "Missing required input"));
  // Bypass does not excuse a stored cycle, even on a generation-only Image edge.
  f.node(bundle)->fixedInputs.at(ports::image) = image(bundle);
  CHECK(message(f.evaluate(mask(bundle)), "cycle"));
  return true;
}

static bool testExecutorCannotReturnInactive() {
  Fixture f; ToyExecutor exec;
  const NodeId generator{"generator"}, effect{"effect"}, bundle{"bundle"};
  CHECK(f.add(generator, NodeOperation::TestReferenceMask) && f.add(effect) &&
        f.add(bundle, NodeOperation::TestMultipleOutputs));
  CHECK(f.connect(image(f.source), input(effect)) && f.connect(image(f.source), input(bundle)));
  for (const auto &endpoint : {mask(generator), image(effect), mask(bundle), image(bundle)}) {
    exec.settings[endpoint.nodeId.value].returnInactive = true;
    const auto result = f.evaluate(endpoint, &exec);
    CHECK(result.state == EvaluationState::Error && message(result, "Executor must not return Inactive") &&
          std::holds_alternative<std::monostate>(result.value));
  }
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
  f.node(crop)->strength = 0;
  CHECK(pixels(f.evaluate(image(crop))) == f.sourcePixels);
  f.node(crop)->strength = .5;
  CHECK(message(f.evaluate(image(crop)), "dimension mismatch"));
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
  const NodeId unavailable{"unavailable-slot-mask"}, partial{"other-slot-mask"};
  CHECK(f.add(unavailable, NodeOperation::TestUnavailableMask) && f.add(partial, NodeOperation::TestReferenceMask));
  exec.settings[partial.value].maskValue = 0.5f;
  CHECK(f.connect(mask(unavailable), input(join, ports::mask, ids[2])) && f.connect(mask(partial), input(join, ports::mask, ids[0])));
  const auto failOpen = f.evaluate(image(join), &exec);
  CHECK(failOpen.state == EvaluationState::Available && pixels(failOpen)->px[0] == 30.5f);
  CHECK(message(failOpen, "using full coverage for Slot 3") && !message(failOpen, "using full coverage for Slot 1"));
  return true;
}

static bool testCancellationAndFailures() {
  Fixture f; ToyExecutor exec;
  const NodeId a{"counted-upstream"}, b{"suppressed-downstream"}, p{"backend"}, warning{"warning-before-cancel"};
  CHECK(f.add(a) && f.add(b) && f.add(p, NodeOperation::ImageProcessor) && f.add(warning, NodeOperation::ImageProcessor));
  CHECK(f.connect(image(f.source), input(warning)) && f.connect(image(warning), input(a)) &&
        f.connect(image(a), input(b)) && f.connect(image(f.source), input(p)));
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
  f.node(p)->strength = .5;
  epoch.store(1); record->duringRender = [&] { epoch.fetch_add(1); };
  result = f.evaluate(image(p), nullptr, token);
  CHECK(result.state == EvaluationState::Cancelled && result.diagnostics.empty() && record->interactive);
  return true;
}

}  // namespace

bool testGraphEvaluator() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"Coordinate mapping defaults, explicit overrides, analytical composition and inverse", testCoordinateMappingMath},
      {"Mask placements start at consumer output, compose crop geometry and preserve pass-through", testCoordinatePlacementChain},
      {"Shared Mask consumers and geometric mixing retain distinct/unknown placements", testSharedCoordinatePlacementsAndMixing},
      {"Step 8 production Image Mask times Strength, exact endpoints, HDR/alpha and dimensions", testProductionMaskAndStrength},
      {"Strength is sampled once for planning, zero dependencies and execution", testPlannedStrength},
      {"Step 8 production mix fan-out executes once and releases intermediates", testProductionMixLifetime},
      {"6C Source sharing, requested endpoints and malformed graph rejection", testSourceAndValidation},
      {"6C linear evaluation, storage independence and reachable-only diagnostics", testLinearStorageAndReachability},
      {"6C fan-out memoization, distinct output bundles and intermediate lifetime", testFanoutBundleAndLifetime},
      {"6D long linear chain releases old intermediates before the last operation", testLongChainRelease},
      {"6C native Processor execution matches serial pixels, alpha and missing/bypass behaviour", testProcessorAndSerialEquivalence},
      {"6C scalar Mask coverage, clamping, Invert, bypass and malformed values", testMaskValidationAndBypass},
      {"6C bypass-aware readiness skips unused incomplete dependencies", testBypassAwareDependencies},
      {"6C Unavailable propagation and optional coverage fail-open warnings", testUnavailableAndFailOpen},
      {"6C Reference frames, generator Inactive bypass and Unavailable distinction", testReferenceAndGeneratorBypass},
      {"Step 8 Inactive unary propagation, optional coverage, fan-out and real black/white Masks", testInactivePropagationAndFanout},
      {"Step 8 per-output bypass skips generation dependencies and preserves missing identities", testPerOutputBypassAndMissingData},
      {"Step 8 executors cannot return Inactive for Image or Mask outputs", testExecutorCannotReturnInactive},
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
