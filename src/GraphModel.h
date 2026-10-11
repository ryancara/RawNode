#pragma once

#include "processors/Processor.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct NodeId {
  std::string value;
  NodeId() = default;
  explicit NodeId(std::string value) : value(std::move(value)) {}
  friend bool operator==(const NodeId &a, const NodeId &b) { return a.value == b.value; }
  friend bool operator!=(const NodeId &a, const NodeId &b) { return !(a == b); }
};

struct SlotId {
  uint64_t value = 0;
  SlotId() = default;
  explicit constexpr SlotId(uint64_t value) : value(value) {}
  friend constexpr bool operator==(SlotId a, SlotId b) { return a.value == b.value; }
  friend constexpr bool operator!=(SlotId a, SlotId b) { return !(a == b); }
};

struct PortId {
  std::string value;
  explicit PortId(std::string value) : value(std::move(value)) {}
  friend bool operator==(const PortId &a, const PortId &b) { return a.value == b.value; }
  friend bool operator!=(const PortId &a, const PortId &b) { return !(a == b); }
  friend bool operator<(const PortId &a, const PortId &b) { return a.value < b.value; }
};

namespace ports {
inline const PortId image{"image"};
inline const PortId mask{"mask"};
inline const PortId reference{"reference"};
}

enum class ValueType { Image, Mask };
enum class PortDirection { Input, Output };

struct OutputRef {
  NodeId nodeId;
  PortId outputPortId;
  friend bool operator==(const OutputRef &a, const OutputRef &b) {
    return a.nodeId == b.nodeId && a.outputPortId == b.outputPortId;
  }
  friend bool operator!=(const OutputRef &a, const OutputRef &b) { return !(a == b); }
};

// An address for commands/lookups, never a second stored edge record.
struct InputAddress {
  NodeId nodeId;
  std::optional<SlotId> slotId;
  PortId inputPortId;
};

struct PortDeclaration {
  PortId id;
  PortDirection direction;
  ValueType type;
  bool required = false;  // Applies only to inputs.
};

struct NodeInterface {
  std::vector<PortDeclaration> fixedPorts;
  std::vector<PortDeclaration> slotInputs;

  const PortDeclaration *findFixedPort(const PortId &id, PortDirection direction) const;
  const PortDeclaration *findSlotInput(const PortId &id) const;
};

enum class NodeOperation {
  Source,
  ImageProcessor,
  // Declarations only; these operations have no production implementation/UI.
  TestImageToMask,
  TestMaskProcessor,
  TestMultipleOutputs,
  TestSlotted,
};

const NodeInterface &nodeInterface(NodeOperation operation);

// Consumers own all incoming references. One input has zero or one producer;
// several consumers can reference the same output without a global edge list.
using InputBindings = std::map<PortId, std::optional<OutputRef>>;

struct SlotSettings {
  bool enabled = true;
};

struct Slot {
  SlotId id;
  InputBindings inputs;
  SlotSettings settings;
};

// The node owns its processor; DocumentMutation protects that lifetime.
struct Node {
  explicit Node(NodeOperation operation = NodeOperation::ImageProcessor);

  NodeId id;
  NodeOperation operation;
  bool enabled = true;
  std::unique_ptr<Processor> processor;
  bool isMissingProcessor() const {
    return operation == NodeOperation::ImageProcessor && !processor;
  }

  InputBindings fixedInputs;
  // Vector position is semantic order. SlotId is node-local identity, never
  // an index. Moving the Slot moves its references and settings together.
  std::vector<Slot> slots;
  Slot makeSlot();
  Slot *findSlot(SlotId id);
  const Slot *findSlot(SlotId id) const;

  // Sidecar V2 processor identity/opaque values survive unavailability.
  std::string storedBackend;
  std::string storedIdentifier;
  std::string storedLabel;
  std::map<std::string, std::string> preservedParamsJson;
  std::map<std::string, bool> groupOpen;

 private:
  uint64_t nextSlotId_ = 1;  // High-water mark survives deletion/reorder.
};
