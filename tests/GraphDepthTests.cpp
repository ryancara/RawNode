#include "GraphDepthTests.h"
#include "GraphDepthFixtures.h"

#include "GraphEvaluator.h"
#include "processors/NativeExposureProcessor.h"

#include <algorithm>
#include <cstdio>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "graph depth check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {
using namespace depth_test;

// Exact expected pixels come from the Source, not a second traversal algorithm.
class Identity final : public graph_test::Executor {
 public:
  size_t calls = 0;
  graph_test::OperationResult execute(const Node &node, const EvaluationContext &,
      const graph_test::InputValues &inputs, const RenderCancellation &) override {
    ++calls;
    graph_test::OperationResult result;
    if (node.operation == NodeOperation::TestMaskProcessor) {
      result.outputs.emplace(ports::mask, inputs.mask);
    } else if (node.operation == NodeOperation::TestReferenceMask) {
      auto mask = std::make_shared<Mask>();
      mask->w = inputs.reference->w; mask->h = inputs.reference->h;
      mask->px.assign(size_t(mask->w) * mask->h, 1.0f);
      result.outputs.emplace(ports::mask, MaskValue(mask));
    } else {
      auto value = inputs.image ? inputs.image : inputs.slots.front().image;
      result.outputs.emplace(ports::image, value);
      if (node.operation == NodeOperation::TestMultipleOutputs) {
        result.outputs.emplace(PortId{"original"}, value);
        auto mask = std::make_shared<Mask>(); mask->w = value->w; mask->h = value->h;
        mask->px.assign(size_t(mask->w) * mask->h, 1.0f);
        result.outputs.emplace(ports::mask, MaskValue(mask));
      }
    }
    return result;
  }
};
ImageValue pixel() {
  auto image = std::make_shared<Image>(); image->w = image->h = 1;
  image->px = {0.25f, -0.5f, 2.0f, 0.375f}; return image;
}
EvaluationResult evaluate(const Chain &chain, Identity *executor = nullptr) {
  return evaluateGraph(chain.view(), chain.output, {EvaluationPurpose::Inspection, 1, 1, 1, 1}, pixel(), {}, executor);
}
bool rejected(const Chain &chain) {
  CHECK(depthError(graphStructureError(chain.view())));
  CHECK(depthError(requestedGraphError(chain.view(), chain.output)));
  Identity executor;
  const auto result = evaluate(chain, &executor);
  CHECK(result.state == EvaluationState::Error && std::holds_alternative<std::monostate>(result.value));
  CHECK(result.diagnostics.size() == 1 && depthError(result.diagnostics.front().message));
  CHECK(executor.calls == 0);
  return true;
}

bool sourceFirstReverseChain() {
  for (size_t depth : {kMaxGraphDependencyDepth, kMaxGraphDependencyDepth + 1}) {
    Chain chain(depth);
    // Source completes before DFS starts at the tail. Only the processors are
    // reversed: the active stack excludes Source and cannot alone count depth.
    std::reverse(chain.nodes.begin() + 1, chain.nodes.end());
    CHECK(chain.nodes.front().id == chain.source && chain.nodes[1].id == chain.output.nodeId);
    if (depth > kMaxGraphDependencyDepth) { CHECK(rejected(chain)); continue; }
    CHECK(graphStructureError(chain.view()).empty() && requestedGraphError(chain.view(), chain.output).empty());
    Identity executor;
    const auto result = evaluate(chain, &executor);
    CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
    CHECK(std::get<ImageValue>(result.value)->px == pixel()->px && executor.calls == depth - 1);
  }
  return true;
}

bool deepBeforeShallow() {
  for (bool slotted : {true, false}) {
    for (size_t tails : {size_t(1), size_t(2)}) {
      // The deep prefix has limit-2 nodes; join adds one, then one/two tails
      // make the longest path exactly limit/limit+1. Both paths share the first
      // processor, independently of the join's other shallow dependencies.
      Chain chain(kMaxGraphDependencyDepth - 2);
      const auto deep = chain.output;
      Node join(slotted ? NodeOperation::TestSlotted : NodeOperation::TestMaskedImage);
      join.id = NodeId{"deep-first-join"};
      if (slotted) {
        for (const auto &producer : {deep, image(id(1))}) {
          auto slot = join.makeSlot(); slot.inputs.at(ports::image) = producer;
          join.slots.push_back(std::move(slot));
        }
      } else {
        Node shallow(NodeOperation::TestReferenceMask); shallow.id = NodeId{"shallow-mask"};
        shallow.fixedInputs.at(ports::reference) = image(id(1));
        // InputBindings orders "image" before "mask": deep then shallow.
        join.fixedInputs.at(ports::image) = deep;
        join.fixedInputs.at(ports::mask) = OutputRef{shallow.id, ports::mask};
        chain.nodes.push_back(std::move(shallow));
      }
      chain.output = image(join.id); chain.nodes.push_back(std::move(join));
      for (size_t i = 0; i < tails; ++i) {
        Node tail(NodeOperation::TestImage); tail.id = NodeId{"tail-" + std::to_string(i)};
        tail.fixedInputs.at(ports::image) = chain.output;
        chain.output = image(tail.id); chain.nodes.push_back(std::move(tail));
      }
      // The join's own longest depth is still supported. A later shallow
      // binding must not erase it before downstream tails extend that path.
      if (tails == 2) { CHECK(rejected(chain)); continue; }
      CHECK(graphStructureError(chain.view()).empty() && requestedGraphError(chain.view(), chain.output).empty());
      Identity executor;
      const auto result = evaluate(chain, &executor);
      CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
      CHECK(std::get<ImageValue>(result.value)->px == pixel()->px && executor.calls == chain.nodes.size() - 1);
    }
  }
  return true;
}

bool boundaryAndStorageOrder() {
  for (size_t depth : {kMaxGraphDependencyDepth - 1, kMaxGraphDependencyDepth, kMaxGraphDependencyDepth + 1}) {
    for (bool reversed : {false, true}) {
      Chain chain(depth);
      if (reversed) std::reverse(chain.nodes.begin(), chain.nodes.end());
      if (depth > kMaxGraphDependencyDepth) { CHECK(rejected(chain)); continue; }
      CHECK(graphStructureError(chain.view()).empty() && requestedGraphError(chain.view(), chain.output).empty());
      Identity executor;
      const auto result = evaluate(chain, &executor);
      CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
      CHECK(std::get<ImageValue>(result.value)->px == pixel()->px && executor.calls == depth - 1);
    }
  }
  // The actual production Processor adapter also executes at the boundary.
  Chain native(kMaxGraphDependencyDepth, NodeOperation::ImageProcessor);
  for (auto &node : native.nodes) if (node.operation != NodeOperation::Source)
    node.processor = std::make_unique<NativeExposureProcessor>();
  const auto result = evaluate(native);
  CHECK(result.state == EvaluationState::Available && result.diagnostics.empty());
  CHECK(std::get<ImageValue>(result.value)->px == pixel()->px);
  return true;
}

bool allStoredDependencies() {
  Chain detached(kMaxGraphDependencyDepth + 1);
  detached.output = image(detached.source);
  CHECK(rejected(detached));
  for (auto &node : detached.nodes) if (node.operation != NodeOperation::Source) node.enabled = false;
  CHECK(rejected(detached));
  for (auto &node : detached.nodes) if (node.operation != NodeOperation::Source) {
    node.operation = NodeOperation::ImageProcessor; node.enabled = true;
    node.storedIdentifier = "unavailable-depth-backend";
  }
  CHECK(rejected(detached)); // Missing processors cannot conceal stored edges.

  Chain slots(1);
  for (size_t i = 1; i <= kMaxGraphDependencyDepth; ++i) {
    Node node(NodeOperation::TestSlotted); node.id = id(i);
    auto shallow = node.makeSlot(); shallow.inputs.at(ports::image) = image(slots.source);
    node.slots.push_back(std::move(shallow));
    auto deep = node.makeSlot(); deep.inputs.at(ports::image) = slots.output;
    node.slots.push_back(std::move(deep));
    slots.output = image(node.id); slots.nodes.push_back(std::move(node));
  }
  CHECK(rejected(slots)); // Only later Slots carry the long chain.
  slots.nodes.pop_back(); slots.output = image(slots.nodes.back().id);
  Identity executor;
  CHECK(graphStructureError(slots.view()).empty());
  CHECK(evaluate(slots, &executor).state == EvaluationState::Available);
  for (auto &node : slots.nodes) if (node.operation == NodeOperation::TestSlotted) {
    node.enabled = false; node.slots.back().settings.enabled = false;
  }
  Node extra(NodeOperation::TestSlotted); extra.id = id(kMaxGraphDependencyDepth);
  auto slot = extra.makeSlot(); slot.inputs.at(ports::image) = slots.output;
  extra.slots.push_back(std::move(slot)); slots.nodes.push_back(std::move(extra));
  CHECK(rejected(slots));

  // A Reference followed by Mask-only dependencies participates in the same bound.
  Chain masks(kMaxGraphDependencyDepth - 1);
  Node reference(NodeOperation::TestReferenceMask); reference.id = NodeId{"reference"};
  reference.fixedInputs.at(ports::reference) = masks.output;
  masks.output = {reference.id, ports::mask}; masks.nodes.push_back(std::move(reference));
  Identity maskExecutor;
  const auto maskResult = evaluate(masks, &maskExecutor);
  CHECK(maskResult.state == EvaluationState::Available && maskResult.valueType == ValueType::Mask);
  CHECK(std::get<MaskValue>(maskResult.value)->px == std::vector<float>{1.0f});
  Node mask(NodeOperation::TestMaskProcessor); mask.id = NodeId{"mask"};
  mask.fixedInputs.at(ports::mask) = masks.output;
  masks.output = {mask.id, ports::mask}; masks.nodes.push_back(std::move(mask));
  CHECK(rejected(masks));
  return true;
}

bool implicitSourceAndBundles() {
  // No stored Source edge on the leaf generator: the evaluator adds one leaf
  // visit. Mask-processing ancestors cannot recursively add another default.
  Chain masks(1);
  Node reference(NodeOperation::TestReferenceMask); reference.id = NodeId{"implicit"};
  masks.output = {reference.id, ports::mask}; masks.nodes.push_back(std::move(reference));
  for (size_t i = 1; i < kMaxGraphDependencyDepth; ++i) {
    Node mask(NodeOperation::TestMaskProcessor); mask.id = id(i);
    mask.fixedInputs.at(ports::mask) = masks.output;
    masks.output = {mask.id, ports::mask}; masks.nodes.push_back(std::move(mask));
  }
  Identity executor;
  CHECK(graphStructureError(masks.view()).empty() && requestedGraphError(masks.view(), masks.output).empty());
  CHECK(evaluate(masks, &executor).state == EvaluationState::Available);
  masks.nodes[1].enabled = false;
  CHECK(evaluate(masks, &executor).state == EvaluationState::Available);

  // Alternate optional coverage and Reference: this nests coverage()/input()
  // as well as visit(), with the extra implicit Source visit at the deepest leaf.
  Chain coverageChain(1);
  for (size_t i = 1; i <= kMaxGraphDependencyDepth; ++i) {
    Node node(i % 2 ? NodeOperation::TestReferenceMask : NodeOperation::TestMaskedImage);
    node.id = id(i);
    if (i % 2) {
      if (i != 1) node.fixedInputs.at(ports::reference) = coverageChain.output;
      coverageChain.output = {node.id, ports::mask};
    } else {
      node.fixedInputs.at(ports::image) = image(coverageChain.source);
      node.fixedInputs.at(ports::mask) = coverageChain.output;
      coverageChain.output = image(node.id);
    }
    coverageChain.nodes.push_back(std::move(node));
  }
  CHECK(graphStructureError(coverageChain.view()).empty());
  CHECK(evaluate(coverageChain, &executor).state == EvaluationState::Available);

  Chain bundle(kMaxGraphDependencyDepth - 1);
  bundle.nodes.back().operation = NodeOperation::TestMultipleOutputs;
  Node join(NodeOperation::TestSlotted); join.id = NodeId{"join"};
  for (const auto &port : {ports::image, PortId{"original"}}) {
    auto slot = join.makeSlot(); slot.inputs.at(ports::image) = OutputRef{bundle.nodes.back().id, port};
    join.slots.push_back(std::move(slot));
  }
  bundle.output = image(join.id); bundle.nodes.push_back(std::move(join));
  Identity bundled;
  CHECK(evaluate(bundle, &bundled).state == EvaluationState::Available && bundled.calls == kMaxGraphDependencyDepth - 1);
  bundle.nodes[bundle.nodes.size() - 2].enabled = false;
  CHECK(evaluate(bundle, &bundled).state == EvaluationState::Available);
  return true;
}

bool cyclesAndWidth() {
  Chain shallow(3);
  shallow.nodes[1].fixedInputs.at(ports::image) = shallow.output;
  CHECK(graphStructureError(shallow.view()) == "Graph contains a cycle");
  Chain cycle(kMaxGraphDependencyDepth);
  cycle.nodes[1].fixedInputs.at(ports::image) = cycle.output;
  CHECK(graphStructureError(cycle.view()) == "Graph contains a cycle");
  std::reverse(cycle.nodes.begin(), cycle.nodes.end());
  CHECK(graphStructureError(cycle.view()) == "Graph contains a cycle");
  // Exercise both early depth refusal and finding a cycle in a large document.
  Chain deep(20000);
  deep.nodes[1].fixedInputs.at(ports::image) = deep.output;
  CHECK(rejected(deep));
  std::reverse(deep.nodes.begin(), deep.nodes.end());
  CHECK(depthError(graphStructureError(deep.view())));
  Chain wide(1);
  for (size_t i = 1; i <= 5000; ++i) {
    Node node(NodeOperation::TestImage); node.id = id(i);
    node.fixedInputs.at(ports::image) = image(wide.source);
    wide.nodes.push_back(std::move(node));
  }
  wide.output = image(wide.nodes.back().id);
  CHECK(graphStructureError(wide.view()).empty());
  Identity executor; CHECK(evaluate(wide, &executor).state == EvaluationState::Available && executor.calls == 1);
  return true;
}
} // namespace

bool testGraphDepth() {
  for (const auto &test : {
      std::make_pair("Source-first reverse chain counts completed children at the exact boundary", sourceFirstReverseChain),
      std::make_pair("deep-first then shallow Slot/fixed inputs retain the longest shared DAG path", deepBeforeShallow),
      std::make_pair("exact node-depth boundary, forward/reverse storage and native evaluation", boundaryAndStorageOrder),
      std::make_pair("disconnected, disabled, unavailable, repeated Slot, Reference and Mask edges", allStoredDependencies),
      std::make_pair("implicit Source, multi-output bundles and dynamic bypass at boundary", implicitSourceAndBundles),
      std::make_pair("shallow/deep cycles and broad shallow graphs", cyclesAndWidth)}) {
    if (!test.second()) {
      std::fprintf(stderr, "graph depth group failed: %s\n", test.first);
      return false;
    }
    std::printf("ok  Graph depth: %s\n", test.first);
  }
  return true;
}
