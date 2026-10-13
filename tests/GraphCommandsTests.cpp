#include "GraphCommandsTests.h"

#include "GraphCommands.h"
#include "GraphValidation.h"
#include "NodeGraph.h"
#include "processors/NativeExposureProcessor.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::fprintf(stderr, "graph command check failed at line %d: %s\n", __LINE__, #expression); \
    return false; \
  } \
} while (false)

namespace {

// Pure topology fixtures have no App, worker or rendering activity. Most node
// IDs intentionally have no relationship to their position in storage.
struct Fixture {
  std::vector<Node> nodes;
  NodeId source{"source-root"};
  OutputRef output{source, ports::image};
  Fixture() {
    Node node(NodeOperation::Source);
    node.id = source;
    nodes.push_back(std::move(node));
  }
  graph::Document document() { return {nodes, source, output}; }
  GraphView view() const { return {nodes, source, output}; }
  bool add(const NodeId &id, NodeOperation operation = NodeOperation::ImageProcessor) {
    Node node(operation);
    node.id = id;
    return graph::addNode(document(), std::move(node)).ok();
  }
  Node *node(const NodeId &id) {
    for (auto &node : nodes) if (node.id == id) return &node;
    return nullptr;
  }
};

static OutputRef image(const NodeId &id) { return {id, ports::image}; }
static OutputRef mask(const NodeId &id) { return {id, ports::mask}; }
static InputAddress input(const NodeId &id, const PortId &port = ports::image,
                          std::optional<SlotId> slot = std::nullopt) { return {id, slot, port}; }

// Exact comparison includes storage order, stable identities, references,
// settings and processor ownership. It is deliberately only a test snapshot,
// not a graph serialization or persistence implementation.
static std::string snapshot(GraphView graph) {
  std::ostringstream out;
  const auto endpoint = [&](const OutputRef &ref) {
    out << std::quoted(ref.nodeId.value) << std::quoted(ref.outputPortId.value);
  };
  const auto bindings = [&](const InputBindings &inputs) {
    out << inputs.size() << '{';
    for (const auto &entry : inputs) {
      out << std::quoted(entry.first.value) << entry.second.has_value();
      if (entry.second) endpoint(*entry.second);
    }
    out << '}';
  };
  out << std::quoted(graph.primarySource.value);
  endpoint(graph.documentOutput);
  for (const auto &node : graph.nodes) {
    out << std::quoted(node.id.value) << static_cast<int>(node.operation) << node.enabled << node.processor.get();
    out << std::quoted(node.storedBackend) << std::quoted(node.storedIdentifier) << std::quoted(node.storedLabel);
    for (const auto &entry : node.preservedParamsJson) out << std::quoted(entry.first) << std::quoted(entry.second);
    for (const auto &entry : node.groupOpen) out << std::quoted(entry.first) << entry.second;
    bindings(node.fixedInputs);
    out << node.slots.size() << '[';
    for (const auto &slot : node.slots) {
      out << slot.id.value << slot.settings.enabled;
      bindings(slot.inputs);
    }
    out << ']';
  }
  return out.str();
}
static std::string snapshot(const Fixture &fixture) { return snapshot(fixture.view()); }

template <typename Command>
static bool rejectedUnchanged(Fixture &fixture, Command command) {
  const auto before = snapshot(fixture);
  const auto result = command();
  CHECK(!result.ok());
  CHECK(!result.addedSlot);
  CHECK(snapshot(fixture) == before);
  return true;
}

static bool bound(Fixture &fixture, const InputAddress &address, const OutputRef &producer) {
  const auto *binding = findInput(fixture.view(), address);
  return binding && *binding == producer;
}
static bool disconnected(Fixture &fixture, const InputAddress &address) {
  const auto *binding = findInput(fixture.view(), address);
  return binding && !*binding;
}

static bool testConnections() {
  Fixture f;
  const NodeId a{"grade-91"}, b{"grade-4"}, analysis{"analysis-73"}, invert{"invert-19"}, slots{"layers-62"};
  CHECK(f.add(a) && f.add(b) && f.add(analysis, NodeOperation::TestImageToMask) &&
        f.add(invert, NodeOperation::TestMaskProcessor) && f.add(slots, NodeOperation::TestSlotted));
  CHECK(graph::connect(f.document(), image(f.source), input(a)).ok());
  CHECK(graph::connect(f.document(), image(a), input(b)).ok());
  CHECK(graph::connect(f.document(), image(a), input(analysis)).ok()); // Image into an Image->Mask operation.
  CHECK(graph::connect(f.document(), mask(analysis), input(invert, ports::mask)).ok());
  CHECK(bound(f, input(b), image(a)) && bound(f, input(analysis), image(a)));
  CHECK(rejectedUnchanged(f, [&] { return graph::connect(f.document(), image(f.source), input(b)); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::connect(f.document(), image(a), input(b)); }));
  CHECK(graph::reconnect(f.document(), image(f.source), input(b)).ok());
  CHECK(bound(f, input(b), image(f.source)));
  CHECK(graph::disconnect(f.document(), input(b)).ok());
  CHECK(disconnected(f, input(b)) && f.node(b)->fixedInputs.size() == 1);
  CHECK(graph::reconnect(f.document(), image(a), input(b)).ok()); // Reconnect may also fill a disconnected input.
  const auto added = graph::addSlot(f.document(), slots);
  CHECK(added.ok() && added.addedSlot);
  CHECK(graph::connect(f.document(), image(a), input(slots, ports::image, added.addedSlot)).ok());
  CHECK(graph::connect(f.document(), mask(invert), input(slots, ports::mask, added.addedSlot)).ok());
  CHECK(graph::disconnect(f.document(), input(slots, ports::image, added.addedSlot)).ok());
  CHECK(disconnected(f, input(slots, ports::image, added.addedSlot)) && f.node(slots)->slots.size() == 1 &&
        f.node(slots)->slots[0].inputs.size() == 2);
  CHECK(graph::setDocumentOutput(f.document(), image(b)).ok());
  CHECK(graphStructureError(f.view()).empty() && requestedGraphError(f.view(), image(b)).empty());
  return true;
}

static bool testEndpointsAndIncompleteTopology() {
  Fixture f;
  const NodeId a{"adjustment"}, m{"mask-data"}, inv{"mask-transform"}, s{"ordered-inputs"}, r{"frame-generator"};
  CHECK(f.add(a) && f.add(m, NodeOperation::TestImageToMask) && f.add(inv, NodeOperation::TestMaskProcessor) &&
        f.add(s, NodeOperation::TestSlotted) && f.add(r, NodeOperation::TestReferenceMask));
  const auto slot = graph::addSlot(f.document(), s).addedSlot;
  CHECK(slot);
  const std::pair<OutputRef, InputAddress> invalid[] = {
      {image(NodeId{"absent"}), input(a)},
      {{f.source, PortId{"absent"}}, input(a)},
      {{r, ports::reference}, input(a)}, // An input port is not an output.
      {image(f.source), input(NodeId{"absent"})},
      {image(f.source), input(a, PortId{"absent"})},
      {image(f.source), input(f.source)}, // Source.image is output-only.
      {image(f.source), input(s, ports::image, SlotId{99})},
      {image(f.source), input(s, ports::reference, slot)},
      {image(f.source), input(inv, ports::mask)},
      {mask(m), input(a)},
      {mask(m), input(s, ports::image, slot)},
      {image(f.source), input(s, ports::mask, slot)},
  };
  for (const auto &entry : invalid) {
    CHECK(rejectedUnchanged(f, [&] { return graph::connect(f.document(), entry.first, entry.second); }));
    CHECK(rejectedUnchanged(f, [&] { return graph::reconnect(f.document(), entry.first, entry.second); }));
  }
  CHECK(rejectedUnchanged(f, [&] { return graph::disconnect(f.document(), input(s, ports::image, SlotId{99})); }));
  for (const auto &output : {mask(m), image(NodeId{"absent"}), OutputRef{a, PortId{"absent"}}})
    CHECK(rejectedUnchanged(f, [&] { return graph::setDocumentOutput(f.document(), output); }));

  // Incomplete nodes remain representable, even when selected as documentOutput.
  CHECK(graphStructureError(f.view()).empty());
  CHECK(requestedGraphError(f.view(), image(f.source)).empty());
  CHECK(!requestedGraphError(f.view(), image(a)).empty());
  CHECK(graph::setDocumentOutput(f.document(), image(a)).ok());
  CHECK(graph::connect(f.document(), image(f.source), input(a)).ok());
  CHECK(requestedGraphError(f.view(), image(a)).empty()); // Unreachable incomplete nodes do not interfere.
  CHECK(requestedGraphError(f.view(), mask(r)).empty()); // Optional Reference absent is valid.
  CHECK(graph::connect(f.document(), image(f.source), input(m)).ok());
  CHECK(requestedGraphError(f.view(), mask(m)).empty()); // Requested output may be Mask.
  CHECK(!requestedGraphError(f.view(), mask(inv)).empty());
  CHECK(graph::connect(f.document(), mask(m), input(inv, ports::mask)).ok());
  CHECK(requestedGraphError(f.view(), mask(inv)).empty());
  CHECK(!requestedGraphError(f.view(), image(s)).empty()); // Required Slot.image is absent.
  CHECK(graph::connect(f.document(), image(a), input(s, ports::image, slot)).ok());
  CHECK(requestedGraphError(f.view(), image(s)).empty()); // Slot.mask and fixed Reference are optional.
  CHECK(graph::disconnect(f.document(), input(a)).ok());
  CHECK(!requestedGraphError(f.view(), image(s)).empty()); // Missing required input upstream.
  CHECK(graph::connect(f.document(), image(a), input(r, ports::reference)).ok());
  CHECK(!requestedGraphError(f.view(), mask(r)).empty()); // Connected optional dependencies are checked.
  CHECK(graph::disconnect(f.document(), input(r, ports::reference)).ok());
  CHECK(requestedGraphError(f.view(), mask(r)).empty());
  CHECK(graph::disconnect(f.document(), input(s, ports::image, slot)).ok());
  CHECK(graph::deleteSlot(f.document(), s, *slot).ok());
  CHECK(requestedGraphError(f.view(), image(s)).empty());
  CHECK(!requestedGraphError(f.view(), {a, PortId{"absent"}}).empty());
  return true;
}

static bool testCycles() {
  Fixture f;
  const NodeId a{"A-90"}, b{"B-17"}, c{"C-3"}, adjustment{"exposure-test"}, radial{"reference-test"},
               s1{"slot-owner-81"}, s2{"slot-owner-26"};
  CHECK(f.add(a) && f.add(b) && f.add(c) && f.add(adjustment, NodeOperation::TestMaskedImage) &&
        f.add(radial, NodeOperation::TestReferenceMask) && f.add(s1, NodeOperation::TestSlotted) &&
        f.add(s2, NodeOperation::TestSlotted));
  CHECK(graph::connect(f.document(), image(f.source), input(a)).ok());
  CHECK(graph::connect(f.document(), image(a), input(b)).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::reconnect(f.document(), image(b), input(a)); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::reconnect(f.document(), image(a), input(a)); }));
  CHECK(graph::connect(f.document(), image(b), input(c)).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::reconnect(f.document(), image(c), input(a)); }));
  f.node(b)->enabled = false;
  CHECK(f.node(a)->isMissingProcessor() && f.node(c)->isMissingProcessor());
  CHECK(rejectedUnchanged(f, [&] { return graph::reconnect(f.document(), image(c), input(a)); }));
  CHECK(graph::connect(f.document(), image(f.source), input(adjustment)).ok());
  CHECK(graph::connect(f.document(), image(adjustment), input(radial, ports::reference)).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::connect(f.document(), mask(radial), input(adjustment, ports::mask)); }));
  CHECK(graph::disconnect(f.document(), input(radial, ports::reference)).ok());
  CHECK(graph::connect(f.document(), mask(radial), input(adjustment, ports::mask)).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::connect(f.document(), image(adjustment), input(radial, ports::reference)); }));
  const auto one = graph::addSlot(f.document(), s1, {false}).addedSlot;
  const auto two = graph::addSlot(f.document(), s2).addedSlot;
  CHECK(one && two);
  CHECK(graph::connect(f.document(), image(s1), input(s2, ports::image, two)).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::connect(f.document(), image(s2), input(s1, ports::image, one)); }));
  CHECK(graph::connect(f.document(), image(s2), input(s1, ports::reference)).error == "Graph contains a cycle");
  CHECK(graphStructureError(f.view()).empty());

  // Deliberately malformed low-level fixture verifies the validator itself,
  // independently of connect's rejection path, including disconnected islands.
  f.node(s1)->slots[0].inputs.at(ports::image) = image(s2);
  CHECK(graphStructureError(f.view()) == "Graph contains a cycle");
  return true;
}

static bool testGenericDeletion() {
  Fixture f;
  const NodeId p{"producer"}, a{"image-consumer"}, b{"original-consumer"}, m{"mask-consumer"}, s{"slot-consumer"}, u{"unrelated"};
  CHECK(f.add(p, NodeOperation::TestMultipleOutputs) && f.add(a) && f.add(b) &&
        f.add(m, NodeOperation::TestMaskProcessor) && f.add(s, NodeOperation::TestSlotted) && f.add(u));
  CHECK(graph::connect(f.document(), image(f.source), input(p)).ok());
  CHECK(graph::connect(f.document(), image(p), input(a)).ok());
  CHECK(graph::connect(f.document(), {p, PortId{"original"}}, input(b)).ok());
  CHECK(graph::connect(f.document(), mask(p), input(m, ports::mask)).ok());
  CHECK(graph::connect(f.document(), image(f.source), input(u)).ok());
  const auto slot = graph::addSlot(f.document(), s, {false}).addedSlot;
  CHECK(slot);
  CHECK(graph::connect(f.document(), image(p), input(s, ports::image, slot)).ok());
  CHECK(graph::connect(f.document(), mask(p), input(s, ports::mask, slot)).ok());
  CHECK(graph::connect(f.document(), image(p), input(s, ports::reference)).ok());
  CHECK(graph::setDocumentOutput(f.document(), {p, PortId{"original"}}).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteNode(f.document(), f.source, image(a)); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteNode(f.document(), p); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteNode(f.document(), p, image(p)); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteNode(f.document(), p, mask(p)); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteNode(f.document(), p, image(NodeId{"absent"})); }));
  CHECK(graph::deleteNode(f.document(), p, image(f.source)).ok());
  CHECK(!f.node(p) && f.output == image(f.source));
  CHECK(disconnected(f, input(a)) && disconnected(f, input(b)) && disconnected(f, input(m, ports::mask)) &&
        disconnected(f, input(s, ports::reference)) && disconnected(f, input(s, ports::image, slot)) &&
        disconnected(f, input(s, ports::mask, slot)));
  CHECK(f.node(s)->slots[0].id == *slot && !f.node(s)->slots[0].settings.enabled);
  CHECK(bound(f, input(u), image(f.source)) && graphStructureError(f.view()).empty());
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteNode(f.document(), NodeId{"absent"}); }));
  CHECK(graph::deleteNode(f.document(), a).ok()); // Unrequested incomplete nodes can be deleted.
  return true;
}

static bool testSlots() {
  Fixture f;
  const NodeId s{"slots-41"}, m{"mask-58"}, ordinary{"ordinary"};
  CHECK(f.add(s, NodeOperation::TestSlotted) && f.add(m, NodeOperation::TestReferenceMask) && f.add(ordinary));
  const auto one = graph::addSlot(f.document(), s, {false}).addedSlot;
  const auto two = graph::addSlot(f.document(), s).addedSlot;
  const auto three = graph::addSlot(f.document(), s).addedSlot;
  CHECK(one == SlotId{1} && two == SlotId{2} && three == SlotId{3});
  CHECK(graph::connect(f.document(), image(f.source), input(s, ports::image, one)).ok());
  CHECK(graph::connect(f.document(), mask(m), input(s, ports::mask, one)).ok());
  CHECK(graph::reorderSlot(f.document(), s, *three, one).ok());
  auto *node = f.node(s);
  CHECK(node->slots[0].id == *three && node->slots[1].id == *one && node->slots[2].id == *two);
  CHECK(!node->slots[1].settings.enabled && bound(f, input(s, ports::image, one), image(f.source)) &&
        bound(f, input(s, ports::mask, one), mask(m)));
  CHECK(graph::reorderSlot(f.document(), s, *one).ok());
  CHECK(node->slots[0].id == *three && node->slots[1].id == *two && node->slots[2].id == *one);
  CHECK(graph::reorderSlot(f.document(), s, *one, one).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::reorderSlot(f.document(), s, *one, SlotId{99}); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::reorderSlot(f.document(), s, SlotId{99}, one); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteSlot(f.document(), s, SlotId{99}); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::addSlot(f.document(), ordinary); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::addSlot(f.document(), NodeId{"absent"}); }));
  CHECK(graph::deleteSlot(f.document(), s, *three).ok());
  CHECK(node->slots[0].id == *two && node->slots[1].id == *one);
  const auto four = graph::addSlot(f.document(), s).addedSlot;
  CHECK(four == SlotId{4});
  CHECK(graph::deleteSlot(f.document(), s, *four).ok());
  CHECK(graph::addSlot(f.document(), s).addedSlot == SlotId{5});
  CHECK(graph::deleteSlot(f.document(), s, *one).ok()); // Deletes the references with the Slot.
  CHECK(!findInput(f.view(), input(s, ports::image, one)) && node->slots[0].id == *two);

  // The only direct Slot insertion is a low-level restored-ID fixture. No graph
  // persistence or public explicitly-ID'd insertion API is introduced.
  const NodeId restored{"restored-slots"};
  Node high(NodeOperation::TestSlotted);
  high.id = restored;
  Slot slot = high.makeSlot();
  slot.id = SlotId{7};
  high.slots.push_back(std::move(slot));
  CHECK(graph::addNode(f.document(), std::move(high)).ok());
  CHECK(graph::addSlot(f.document(), restored).addedSlot == SlotId{8});
  CHECK(graph::deleteSlot(f.document(), restored, SlotId{8}).ok());
  CHECK(graph::deleteSlot(f.document(), restored, SlotId{7}).ok());
  CHECK(graph::addSlot(f.document(), restored).addedSlot == SlotId{9});

  // Also observe a restored ID before deleting it, before any allocation.
  const NodeId deletedHigh{"restored-then-deleted"};
  CHECK(f.add(deletedHigh, NodeOperation::TestSlotted));
  Slot restoredSlot = f.node(deletedHigh)->makeSlot();
  restoredSlot.id = SlotId{7};
  f.node(deletedHigh)->slots.push_back(std::move(restoredSlot));
  CHECK(graph::deleteSlot(f.document(), deletedHigh, SlotId{7}).ok());
  CHECK(graph::addSlot(f.document(), deletedHigh).addedSlot == SlotId{8});
  const auto next = graph::addSlot(f.document(), s).addedSlot;
  CHECK(next == SlotId{6}); // Failed commands did not consume an ID.
  return true;
}

static bool testSplices() {
  Fixture f;
  const NodeId a{"A-95"}, b{"B-12"}, n{"inserted-72"}, c{"tail-31"}, multi{"multi-tail"},
               fan{"fan-out"}, original{"secondary"}, maskConsumer{"mask-consumer"},
               reference{"reference-consumer"}, slots{"slot-consumer"};
  Node first; first.id = a;
  CHECK(graph::insertImageNode(f.document(), std::move(first)).ok());
  Node second; second.id = b;
  CHECK(graph::insertImageNode(f.document(), std::move(second)).ok());
  Node middle; middle.id = n;
  CHECK(graph::insertImageNode(f.document(), std::move(middle), input(b)).ok());
  CHECK(bound(f, input(a), image(f.source)) && bound(f, input(n), image(a)) && bound(f, input(b), image(n)));
  CHECK(f.output == image(b) && f.nodes[2].id == b && f.nodes[3].id == n); // Appending storage did not define topology.
  Node tail; tail.id = c;
  CHECK(graph::insertImageNode(f.document(), std::move(tail)).ok());
  CHECK(bound(f, input(c), image(b)) && f.output == image(c));
  Node many(NodeOperation::TestMultipleOutputs); many.id = multi;
  CHECK(graph::insertImageNode(f.document(), std::move(many)).ok());
  CHECK(f.add(fan) && f.add(original) && f.add(maskConsumer, NodeOperation::TestMaskProcessor) &&
        f.add(reference, NodeOperation::TestReferenceMask) && f.add(slots, NodeOperation::TestSlotted));
  const auto slot = graph::addSlot(f.document(), slots).addedSlot;
  CHECK(slot);
  CHECK(graph::connect(f.document(), image(multi), input(reference, ports::reference)).ok());
  CHECK(graph::connect(f.document(), image(multi), input(slots, ports::image, slot)).ok());
  CHECK(graph::connect(f.document(), image(multi), input(fan)).ok());
  CHECK(graph::connect(f.document(), {multi, PortId{"original"}}, input(original)).ok());
  CHECK(graph::connect(f.document(), mask(multi), input(maskConsumer, ports::mask)).ok());
  CHECK(graph::setDocumentOutput(f.document(), {multi, PortId{"original"}}).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteImageNode(f.document(), multi); }));
  CHECK(graph::setDocumentOutput(f.document(), image(multi)).ok());
  CHECK(graph::deleteImageNode(f.document(), multi).ok());
  CHECK(f.output == image(c) && bound(f, input(fan), image(c)) && disconnected(f, input(original)) &&
        disconnected(f, input(maskConsumer, ports::mask)));
  CHECK(bound(f, input(reference, ports::reference), image(c)) && bound(f, input(slots, ports::image, slot), image(c)));
  CHECK(graph::deleteImageNode(f.document(), n).ok());
  CHECK(bound(f, input(b), image(a)) && !f.node(n));
  CHECK(graph::deleteImageNode(f.document(), c).ok());
  CHECK(f.output == image(b) && bound(f, input(fan), image(b)));
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteImageNode(f.document(), f.source); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::deleteImageNode(f.document(), original); })); // No Image producer to substitute.
  CHECK(graphStructureError(f.view()).empty());
  return true;
}

static bool testMoveAndAtomicSplices() {
  Fixture f;
  const NodeId a{"A-88"}, b{"B-11"}, c{"C-59"}, branch{"branch"}, radial{"frame-dependency"};
  for (const auto &id : {a, b, c}) {
    Node node; node.id = id;
    CHECK(graph::insertImageNode(f.document(), std::move(node)).ok());
  }
  CHECK(f.add(branch, NodeOperation::TestReferenceMask));
  CHECK(graph::connect(f.document(), image(c), input(branch, ports::reference)).ok());
  std::reverse(f.nodes.begin(), f.nodes.end());
  std::vector<NodeId> storage;
  for (const auto &node : f.nodes) storage.push_back(node.id);
  CHECK(graph::moveImageNode(f.document(), c, a).ok());
  CHECK(bound(f, input(c), image(f.source)) && bound(f, input(a), image(c)) && bound(f, input(b), image(a)) &&
        f.output == image(b) && bound(f, input(branch, ports::reference), image(c)));
  CHECK(graph::moveImageNode(f.document(), c).ok());
  CHECK(bound(f, input(a), image(f.source)) && bound(f, input(b), image(a)) && bound(f, input(c), image(b)) && f.output == image(c));
  for (size_t i = 0; i < storage.size(); ++i) CHECK(storage[i] == f.nodes[i].id);
  CHECK(rejectedUnchanged(f, [&] { return graph::moveImageNode(f.document(), a, f.source); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::moveImageNode(f.document(), f.source, a); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::moveImageNode(f.document(), branch, a); }));
  CHECK(rejectedUnchanged(f, [&] { return graph::moveImageNode(f.document(), a, NodeId{"absent"}); }));
  CHECK(graph::moveImageNode(f.document(), b, b).ok());

  CHECK(f.add(radial, NodeOperation::TestReferenceMask));
  CHECK(graph::connect(f.document(), image(b), input(radial, ports::reference)).ok());
  Node cyclic(NodeOperation::TestMaskedImage);
  cyclic.id = NodeId{"cyclic-insert"};
  // Low-level detached candidate initialization: final insertion closes the
  // mask->Reference->image cycle only after both splice edges have been staged.
  cyclic.fixedInputs.at(ports::mask) = mask(radial);
  CHECK(rejectedUnchanged(f, [&] { return graph::insertImageNode(f.document(), std::move(cyclic), input(b)); }));
  CHECK(cyclic.id == NodeId{"cyclic-insert"} && !cyclic.fixedInputs.at(ports::image));
  Node wrong(NodeOperation::TestImageToMask); wrong.id = NodeId{"not-unary"};
  CHECK(rejectedUnchanged(f, [&] { return graph::insertImageNode(f.document(), std::move(wrong), input(b)); }));
  Node ordinary; ordinary.id = NodeId{"pending-processor"};
  ordinary.processor = std::make_unique<NativeExposureProcessor>();
  auto *owned = ordinary.processor.get();
  CHECK(rejectedUnchanged(f, [&] { return graph::insertImageNode(f.document(), std::move(ordinary), input(branch)); }));
  CHECK(ordinary.processor.get() == owned && ordinary.id == NodeId{"pending-processor"});
  CHECK(graph::insertImageNode(f.document(), std::move(ordinary), input(b)).ok());
  CHECK(f.node(NodeId{"pending-processor"})->processor.get() == owned);
  Node malformed; malformed.id = NodeId{"missing-storage"}; malformed.fixedInputs.clear();
  CHECK(rejectedUnchanged(f, [&] { return graph::insertImageNode(f.document(), std::move(malformed)); }));

  // Moving an Image node above a dependency supplied by an off-spine Reference
  // would close a cycle. Optional inputs and fan-out stay represented.
  Fixture joined;
  const NodeId x{"X"}, y{"Y"}, r{"R"};
  Node xn; xn.id = x;
  Node yn(NodeOperation::TestMaskedImage); yn.id = y;
  CHECK(graph::insertImageNode(joined.document(), std::move(xn)).ok());
  CHECK(graph::insertImageNode(joined.document(), std::move(yn)).ok());
  CHECK(joined.add(r, NodeOperation::TestReferenceMask));
  CHECK(graph::connect(joined.document(), image(x), input(r, ports::reference)).ok());
  CHECK(graph::connect(joined.document(), mask(r), input(y, ports::mask)).ok());
  CHECK(rejectedUnchanged(joined, [&] { return graph::moveImageNode(joined.document(), y, x); }));
  return true;
}

static bool testSpineRejections() {
  Fixture f;
  const NodeId a{"unary"}, multi{"multiple-outputs"}, join{"non-unary"};
  Node ordinary; ordinary.id = a;
  Node outputs(NodeOperation::TestMultipleOutputs); outputs.id = multi;
  CHECK(graph::insertImageNode(f.document(), std::move(ordinary)).ok());
  CHECK(graph::insertImageNode(f.document(), std::move(outputs)).ok());
  CHECK(graph::setDocumentOutput(f.document(), {multi, PortId{"original"}}).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::moveImageNode(f.document(), a); }));
  CHECK(f.add(join, NodeOperation::TestSlotted));
  const auto slot = graph::addSlot(f.document(), join).addedSlot;
  CHECK(slot && graph::connect(f.document(), image(multi), input(join, ports::image, slot)).ok());
  CHECK(graph::setDocumentOutput(f.document(), image(join)).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::moveImageNode(f.document(), a); }));
  CHECK(graph::setDocumentOutput(f.document(), image(multi)).ok());
  CHECK(graph::disconnect(f.document(), input(multi)).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::moveImageNode(f.document(), a); }));
  return true;
}

static bool testMalformedStateAndOwnership() {
  Fixture f;
  const NodeId a{"processor"}, s{"slotted"}, m{"mask-source"};
  CHECK(f.add(a) && f.add(s, NodeOperation::TestSlotted) && f.add(m, NodeOperation::TestReferenceMask));
  Node duplicate; duplicate.id = a;
  CHECK(rejectedUnchanged(f, [&] { return graph::addNode(f.document(), std::move(duplicate)); }));
  Node empty;
  CHECK(rejectedUnchanged(f, [&] { return graph::addNode(f.document(), std::move(empty)); }));
  Node source(NodeOperation::Source); source.id = NodeId{"second-source"};
  CHECK(rejectedUnchanged(f, [&] { return graph::addNode(f.document(), std::move(source)); }));
  Node native(NodeOperation::TestSlotted); native.id = NodeId{"bad-owner"};
  native.processor = std::make_unique<NativeExposureProcessor>();
  CHECK(rejectedUnchanged(f, [&] { return graph::addNode(f.document(), std::move(native)); }));
  CHECK(native.processor);
  f.node(s)->processor = std::make_unique<NativeExposureProcessor>();
  CHECK(graphStructureError(f.view()) == "Graph-native node has an Image processor");
  f.node(s)->processor.reset();
  Slot illegal{SlotId{1}, {}, {}};
  f.node(a)->slots.push_back(illegal);
  CHECK(graphStructureError(f.view()) == "Node has no repeated inputs");
  f.node(a)->slots.clear();
  for (const auto &bad : {image(NodeId{"absent"}), OutputRef{f.source, PortId{"absent"}}, mask(m)}) {
    f.node(a)->fixedInputs.at(ports::image) = bad;
    CHECK(!graphStructureError(f.view()).empty());
  }
  f.node(a)->fixedInputs.at(ports::image) = std::nullopt;
  f.node(a)->operation = static_cast<NodeOperation>(999);
  CHECK(graphStructureError(f.view()) == "Unknown graph operation");
  f.node(a)->operation = NodeOperation::ImageProcessor;
  Slot exhausted = f.node(s)->makeSlot();
  exhausted.id = SlotId{std::numeric_limits<uint64_t>::max()};
  f.node(s)->slots.push_back(std::move(exhausted));
  CHECK(rejectedUnchanged(f, [&] { return graph::addSlot(f.document(), s); }));
  CHECK(graph::deleteSlot(f.document(), s, SlotId{std::numeric_limits<uint64_t>::max()}).ok());
  CHECK(rejectedUnchanged(f, [&] { return graph::addSlot(f.document(), s); })); // Exhaustion survives deleting highest ID.
  return true;
}

static bool testAppAndSourceMove() {
  App app;
  const NodeId a{"live-processor"}, b{"live-consumer"}, slots{"live-slots"};
  Node node; node.id = a;
  node.processor = std::make_unique<NativeExposureProcessor>();
  auto *owned = node.processor.get();
  CHECK(graph::insertImageNode(app, std::move(node)).ok());
  CHECK(findNode(app, a)->processor.get() == owned);
  Node consumer; consumer.id = b;
  CHECK(graph::addNode(app, std::move(consumer)).ok());
  CHECK(graph::connect(app, image(a), input(b)).ok());
  const auto beforeRejected = snapshot({app.nodes, app.primarySource, app.documentOutput});
  auto *consumerProcessor = findNode(app, a)->processor.get();
  CHECK(!graph::connect(app, image(b), input(a)).ok()); // Would close a cycle.
  CHECK(snapshot({app.nodes, app.primarySource, app.documentOutput}) == beforeRejected &&
        findNode(app, a)->processor.get() == consumerProcessor);
  CHECK(graph::reconnect(app, image(app.primarySource), input(b)).ok());
  CHECK(graph::disconnect(app, input(b)).ok());
  Node slotted(NodeOperation::TestSlotted); slotted.id = slots;
  CHECK(graph::addNode(app, std::move(slotted)).ok());
  const auto one = graph::addSlot(app, slots).addedSlot;
  const auto two = graph::addSlot(app, slots).addedSlot;
  CHECK(one && two && graph::reorderSlot(app, slots, *two, one).ok());
  CHECK(graph::deleteSlot(app, slots, *one).ok());
  CHECK(graph::setDocumentOutput(app, image(a)).ok());
  CHECK(graph::moveImageNode(app, a).ok());
  selectNode(app, app.nodes[2].id);
  CHECK(graph::deleteNode(app, a, image(app.primarySource)).ok());
  CHECK(app.selectedNodeId == app.nodes[1].id && app.nodes[1].id == b && !findNode(app, a));
  CHECK(graph::deleteNode(app, b).ok() && !app.selectedNodeId);
  CHECK(documentStateError(app).empty());
  Node tail; tail.id = NodeId{"live-tail"};
  CHECK(graph::insertImageNode(app, std::move(tail)).ok());
  CHECK(graph::deleteImageNode(app, NodeId{"live-tail"}).ok());

  App serial;
  CHECK(addNativeExposureNode(serial) && addNativeCstNode(serial));
  const auto first = serial.nodes[1].id, second = serial.nodes[2].id;
  moveNode(serial, serial.nodes[1].id, serial.nodes[0].id);
  CHECK(serial.nodes[0].id == serial.primarySource && serial.nodes[1].id == first && serial.nodes[2].id == second);
  moveNode(serial, serial.nodes[0].id, serial.nodes[2].id);
  CHECK(serial.nodes[0].id == serial.primarySource && serial.nodes[1].id == first);
  std::swap(serial.nodes[0], serial.nodes[1]); // Source now separates processors.
  moveNode(serial, serial.nodes[0].id, serial.nodes[2].id);
  CHECK(serial.nodes[0].id == first && serial.nodes[1].id == serial.primarySource && serial.nodes[2].id == second);
  CHECK(!graph::moveImageNode(serial, first, serial.primarySource).ok());
  // Active add/move/load semantics now produce one authoritative topology.
  CHECK(serial.nodes[0].fixedInputs.at(ports::image) == image(second) &&
        serial.nodes[2].fixedInputs.at(ports::image) == image(serial.primarySource) && serial.documentOutput == image(first));
  const auto chain = captureChain(serial);
  applyChain(serial, chain);
  CHECK(serial.documentOutput == image(first) && serial.nodes[1].id == second && serial.nodes[2].id == first &&
        serial.nodes[1].fixedInputs.at(ports::image) == image(serial.primarySource) &&
        serial.nodes[2].fixedInputs.at(ports::image) == image(second));
  return true;
}

}  // namespace

bool testGraphCommands() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"6B consumer-owned connect/reconnect/disconnect and fan-out", testConnections},
      {"6B endpoints, types, required/optional inputs and editable incompleteness", testEndpointsAndIncompleteTopology},
      {"6B fixed, Reference, Slot and disabled/unavailable dependency cycles", testCycles},
      {"6B generic deletion and explicit documentOutput replacement", testGenericDeletion},
      {"6B Slot identity, order, settings, references and restored high-water IDs", testSlots},
      {"6B unary Image insert/delete splices and secondary-output consumers", testSplices},
      {"6B topology moves, storage independence and compound failure atomicity", testMoveAndAtomicSplices},
      {"6B spine moves refuse secondary outputs, non-unary joins and disconnected inputs", testSpineRejections},
      {"6B malformed operation/storage/ownership and Slot exhaustion", testMalformedStateAndOwnership},
      {"6B App mutation wrappers and Source move boundaries", testAppAndSourceMove},
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
