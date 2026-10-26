#include "GraphModel.h"

#include <limits>
#include <set>
#include <stdexcept>

const NodeInterface &nodeInterface(NodeOperation operation) {
  using Direction = PortDirection;
  static const NodeInterface source{{
      {ports::image, Direction::Output, ValueType::Image},
  }, {}};
  static const NodeInterface processor{{
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::image, Direction::Output, ValueType::Image, false, ports::image},
  }, {}};
  static const NodeInterface imageToMask{{
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::mask, Direction::Output, ValueType::Mask},
  }, {}};
  static const NodeInterface maskProcessor{{
      {ports::mask, Direction::Input, ValueType::Mask, true},
      {ports::mask, Direction::Output, ValueType::Mask, false, ports::mask},
  }, {}};
  static const NodeInterface multipleOutputs{{
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::image, Direction::Output, ValueType::Image, false, ports::image},
      {PortId{"original"}, Direction::Output, ValueType::Image, false, ports::image},
      {ports::mask, Direction::Output, ValueType::Mask},
  }, {}};
  static const NodeInterface slotted{{
      {ports::reference, Direction::Input, ValueType::Image, false},
      {ports::image, Direction::Output, ValueType::Image},
  }, {
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::mask, Direction::Input, ValueType::Mask, false},
  }};
  static const NodeInterface maskedImage{{
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::mask, Direction::Input, ValueType::Mask, false},
      {ports::image, Direction::Output, ValueType::Image, false, ports::image},
  }, {}};
  static const NodeInterface referenceMask{{
      {ports::reference, Direction::Input, ValueType::Image, false},
      {ports::mask, Direction::Output, ValueType::Mask},
  }, {}};
  static const NodeInterface unavailableMask{{
      {ports::mask, Direction::Output, ValueType::Mask},
  }, {}};
  switch (operation) {
    case NodeOperation::Source: return source;
    case NodeOperation::ImageProcessor: return processor;
    case NodeOperation::TestImageToMask: return imageToMask;
    case NodeOperation::TestMaskProcessor: return maskProcessor;
    case NodeOperation::TestMultipleOutputs: return multipleOutputs;
    case NodeOperation::TestSlotted: return slotted;
    case NodeOperation::TestMaskedImage: return maskedImage;
    case NodeOperation::TestReferenceMask: return referenceMask;
    case NodeOperation::TestImage: return processor;
    case NodeOperation::TestUnavailableImage: return source;
    case NodeOperation::TestUnavailableMask: return unavailableMask;
  }
  throw std::invalid_argument("Unknown graph operation");
}

const PortDeclaration *NodeInterface::findFixedPort(const PortId &id, PortDirection direction) const {
  for (const auto &port : fixedPorts)
    if (port.id == id && port.direction == direction) return &port;
  return nullptr;
}

const PortDeclaration *NodeInterface::findSlotInput(const PortId &id) const {
  for (const auto &port : slotInputs)
    if (port.id == id) return &port;
  return nullptr;
}

Node::Node(NodeOperation operation) : operation(operation) {
  for (const auto &port : nodeInterface(operation).fixedPorts)
    if (port.direction == PortDirection::Input) fixedInputs.emplace(port.id, std::nullopt);
}

Slot Node::makeSlot() {
  const auto &declarations = nodeInterface(operation).slotInputs;
  if (declarations.empty()) throw std::logic_error("Node has no repeated inputs");
  // Also honour explicitly restored/staged IDs without deriving identity from
  // vector position. The high-water mark prevents reuse after deletion.
  observeSlotIds();
  if (nextSlotId_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Slot IDs exhausted");
  Slot slot{SlotId{nextSlotId_++}, {}, {}};
  for (const auto &port : declarations) slot.inputs.emplace(port.id, std::nullopt);
  return slot;
}

uint64_t Node::nextSlotIdForPersistence() const {
  uint64_t next = nextSlotId_;
  for (const auto &slot : slots)
    if (next <= slot.id.value)
      next = slot.id.value == std::numeric_limits<uint64_t>::max() ? slot.id.value : slot.id.value + 1;
  return next;
}

bool Node::restoreNextSlotId(uint64_t next) {
  if (!next || nodeInterface(operation).slotInputs.empty()) return false;
  std::set<uint64_t> ids;
  for (const auto &slot : slots) {
    if (!slot.id.value || !ids.insert(slot.id.value).second) return false;
    if (next <= slot.id.value && next != std::numeric_limits<uint64_t>::max()) return false;
  }
  nextSlotId_ = next;
  return true;
}

void Node::observeSlotIds() {
  nextSlotId_ = nextSlotIdForPersistence();
}

Slot *Node::findSlot(SlotId id) {
  for (auto &slot : slots)
    if (slot.id == id) return &slot;
  return nullptr;
}

const Slot *Node::findSlot(SlotId id) const {
  for (const auto &slot : slots)
    if (slot.id == id) return &slot;
  return nullptr;
}

const Node *findNode(GraphView graph, const NodeId &id) {
  for (const auto &node : graph.nodes)
    if (node.id == id) return &node;
  return nullptr;
}

const PortDeclaration *findOutput(GraphView graph, const OutputRef &output) {
  const Node *node = findNode(graph, output.nodeId);
  return node ? nodeInterface(node->operation).findFixedPort(output.outputPortId, PortDirection::Output) : nullptr;
}

const PortDeclaration *findInputDeclaration(GraphView graph, const InputAddress &input) {
  const Node *node = findNode(graph, input.nodeId);
  if (!node) return nullptr;
  const auto &interface = nodeInterface(node->operation);
  if (input.slotId)
    return node->findSlot(*input.slotId) ? interface.findSlotInput(input.inputPortId) : nullptr;
  return interface.findFixedPort(input.inputPortId, PortDirection::Input);
}

const std::optional<OutputRef> *findInput(GraphView graph, const InputAddress &input) {
  if (!findInputDeclaration(graph, input)) return nullptr;
  const Node *node = findNode(graph, input.nodeId);
  const auto &bindings = input.slotId ? node->findSlot(*input.slotId)->inputs : node->fixedInputs;
  const auto found = bindings.find(input.inputPortId);
  return found == bindings.end() ? nullptr : &found->second;
}
