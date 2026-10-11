#include "GraphModel.h"

#include <limits>
#include <stdexcept>

const NodeInterface &nodeInterface(NodeOperation operation) {
  using Direction = PortDirection;
  static const NodeInterface source{{
      {ports::image, Direction::Output, ValueType::Image},
  }, {}};
  static const NodeInterface processor{{
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::image, Direction::Output, ValueType::Image},
  }, {}};
  static const NodeInterface imageToMask{{
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::mask, Direction::Output, ValueType::Mask},
  }, {}};
  static const NodeInterface maskProcessor{{
      {ports::mask, Direction::Input, ValueType::Mask, true},
      {ports::mask, Direction::Output, ValueType::Mask},
  }, {}};
  static const NodeInterface multipleOutputs{{
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::image, Direction::Output, ValueType::Image},
      {PortId{"original"}, Direction::Output, ValueType::Image},
      {ports::mask, Direction::Output, ValueType::Mask},
  }, {}};
  static const NodeInterface slotted{{
      {ports::reference, Direction::Input, ValueType::Image, false},
      {ports::image, Direction::Output, ValueType::Image},
  }, {
      {ports::image, Direction::Input, ValueType::Image, true},
      {ports::mask, Direction::Input, ValueType::Mask, false},
  }};
  switch (operation) {
    case NodeOperation::Source: return source;
    case NodeOperation::ImageProcessor: return processor;
    case NodeOperation::TestImageToMask: return imageToMask;
    case NodeOperation::TestMaskProcessor: return maskProcessor;
    case NodeOperation::TestMultipleOutputs: return multipleOutputs;
    case NodeOperation::TestSlotted: return slotted;
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
  for (const auto &slot : slots) {
    if (slot.id.value == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("Slot IDs exhausted");
    if (nextSlotId_ <= slot.id.value) nextSlotId_ = slot.id.value + 1;
  }
  if (nextSlotId_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Slot IDs exhausted");
  Slot slot{SlotId{nextSlotId_++}, {}, {}};
  for (const auto &port : declarations) slot.inputs.emplace(port.id, std::nullopt);
  return slot;
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
