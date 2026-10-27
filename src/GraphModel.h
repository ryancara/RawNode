#pragma once

#include "processors/Processor.h"

#include <atomic>
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
  // Output-only: the same-type fixed input returned on bypass. Mask outputs
  // without a pass-through become Inactive; generation inputs are not needed.
  std::optional<PortId> bypassInput;
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
  // Static test interfaces only; pixel implementations require the evaluator's
  // test executor. These are never production UI/persistence features.
  TestImageToMask,
  TestMaskProcessor,
  TestMultipleOutputs,
  TestSlotted,
  TestMaskedImage,
  TestReferenceMask,
  TestImage,
  TestUnavailableImage,
  TestUnavailableMask,
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

namespace graph_detail { class StagedEdit; }

// One atomic scalar, movable so Node remains a value in the document vector.
// It publishes no other state; evaluation samples it once during planning.
class NodeStrength {
 public:
  NodeStrength() = default;
  NodeStrength(NodeStrength &&other) noexcept : value_(double(other)) {}
  NodeStrength &operator=(NodeStrength &&other) noexcept { return *this = double(other); }
  operator double() const noexcept { return value_.load(std::memory_order_relaxed); }
  NodeStrength &operator=(double value) noexcept {
    value_.store(value, std::memory_order_relaxed);
    return *this;
  }
 private:
  std::atomic<double> value_{1.0};
};

// The node owns its processor; DocumentMutation protects that lifetime.
struct Node {
  explicit Node(NodeOperation operation = NodeOperation::ImageProcessor);

  NodeId id;
  NodeOperation operation;
  bool enabled = true;
  NodeStrength strength;
  std::unique_ptr<Processor> processor;
  bool isMissingProcessor() const {
    return operation == NodeOperation::ImageProcessor && !processor;
  }

  InputBindings fixedInputs;
  // Vector position is semantic order. SlotId is node-local identity, never
  // an index. Moving the Slot moves its references and settings together.
  std::vector<Slot> slots;
  Slot makeSlot();
  // Capture observes live IDs as staging/allocation does. Restore checks the
  // supplied high-water mark against all live IDs, including exhaustion.
  uint64_t nextSlotIdForPersistence() const;
  bool restoreNextSlotId(uint64_t next);
  Slot *findSlot(SlotId id);
  const Slot *findSlot(SlotId id) const;

  // Sidecar V2 processor identity/opaque values survive unavailability.
  std::string storedBackend;
  std::string storedIdentifier;
  std::string storedLabel;
  std::map<std::string, std::string> preservedParamsJson;
  std::map<std::string, bool> groupOpen;

 private:
  friend class graph_detail::StagedEdit;
  void observeSlotIds();
  uint64_t nextSlotId_ = 1;  // High-water mark survives deletion/reorder.
};

// Borrowed document storage, not a second graph. Validation is worker-free.
struct GraphView {
  const std::vector<Node> &nodes;
  const NodeId &primarySource;
  const OutputRef &documentOutput;
};

const Node *findNode(GraphView graph, const NodeId &id);
const PortDeclaration *findOutput(GraphView graph, const OutputRef &output);
const PortDeclaration *findInputDeclaration(GraphView graph, const InputAddress &input);
const std::optional<OutputRef> *findInput(GraphView graph, const InputAddress &input);
