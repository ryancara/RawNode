#pragma once

#include "GraphModel.h"

struct App;

namespace graph {

// Detached documents can use these pure commands directly. Live App callers
// must use the App overloads, which own DocumentMutation execution exclusion.
// Both overloads use the same stage/validate/commit implementation.
struct Document {
  std::vector<Node> &nodes;
  const NodeId &primarySource;
  OutputRef &documentOutput;
  operator GraphView() const { return {nodes, primarySource, documentOutput}; }
};

struct CommandResult {
  std::string error;
  std::optional<SlotId> addedSlot;
  bool ok() const { return error.empty(); }
};

CommandResult connect(Document document, const OutputRef &producer, const InputAddress &destination);
CommandResult reconnect(Document document, const OutputRef &producer, const InputAddress &destination);
CommandResult disconnect(Document document, const InputAddress &destination);
// The caller supplies a fresh nonempty NodeId and an owned, detached node. The
// node (including its processor) is moved only on successful commit.
CommandResult addNode(Document document, Node &&node);
CommandResult deleteNode(Document document, const NodeId &node,
                         std::optional<OutputRef> replacementOutput = std::nullopt);
CommandResult setDocumentOutput(Document document, const OutputRef &output);
// New Slots have declared, disconnected inputs. Settings belong to the Slot.
CommandResult addSlot(Document document, const NodeId &node, SlotSettings settings = {});
CommandResult deleteSlot(Document document, const NodeId &node, SlotId slot);
// Move before another SlotId, or to the end with nullopt. No index identity.
CommandResult reorderSlot(Document document, const NodeId &node, SlotId slot,
                          std::optional<SlotId> before = std::nullopt);
// Insert into an existing fixed image input, or append to documentOutput when
// before is absent. The new node must have a required image input/image output.
CommandResult insertImageNode(Document document, Node &&node,
                              std::optional<InputAddress> before = std::nullopt);
CommandResult deleteImageNode(Document document, const NodeId &node);
// Derive the unary spine from documentOutput to Source and rewrite its image
// edges. Off-spine consumers keep their references; storage order never moves.
CommandResult moveImageNode(Document document, const NodeId &node,
                            std::optional<NodeId> before = std::nullopt);

CommandResult connect(App &app, const OutputRef &producer, const InputAddress &destination);
CommandResult reconnect(App &app, const OutputRef &producer, const InputAddress &destination);
CommandResult disconnect(App &app, const InputAddress &destination);
CommandResult addNode(App &app, Node &&node);
CommandResult deleteNode(App &app, const NodeId &node,
                         std::optional<OutputRef> replacementOutput = std::nullopt);
CommandResult setDocumentOutput(App &app, const OutputRef &output);
CommandResult addSlot(App &app, const NodeId &node, SlotSettings settings = {});
CommandResult deleteSlot(App &app, const NodeId &node, SlotId slot);
CommandResult reorderSlot(App &app, const NodeId &node, SlotId slot,
                          std::optional<SlotId> before = std::nullopt);
CommandResult insertImageNode(App &app, Node &&node,
                              std::optional<InputAddress> before = std::nullopt);
CommandResult deleteImageNode(App &app, const NodeId &node);
CommandResult moveImageNode(App &app, const NodeId &node,
                            std::optional<NodeId> before = std::nullopt);

}  // namespace graph
