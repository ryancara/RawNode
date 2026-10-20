#pragma once

#include "GraphModel.h"

struct App;

namespace graph {

// Detached documents can use these pure commands directly. Mutating live App
// callers must use the App overloads, which own DocumentMutation exclusion.
// Both overloads use the same stage/validate/commit implementation. Topology-only
// validation queries may borrow live storage on the control thread.
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

// Image-spine projection derived from topology;
// excludes Source. Structurally invalid graphs return no spine. If traversal
// stops at a non-unary node or disconnected input, the recognized unary suffix
// is returned in upstream-to-output order for display. Reordering and linear
// capture require an empty error.
std::string imageSpine(GraphView graph, std::vector<NodeId> &spine);
bool isUnaryImageNode(const Node &node);

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
// Topology-only preflight for a new ordinary ImageProcessor, before backend
// construction enters execution exclusion. A branch binds only the new input
// and preserves Output/consumers; otherwise use the insertImageNode splice.
// Uses the same staging/validation and never commits or acquires mutation.
CommandResult validateImageNodeInsertion(Document document,
                                        std::optional<InputAddress> before = std::nullopt,
                                        std::optional<OutputRef> branch = std::nullopt);
// Atomic linear batch splice. Nodes are ordered by their internal Image edges,
// with a detached entry. Validate the whole destination and selected spine point
// before moving any processor. Caller owns execution exclusion for live storage.
CommandResult insertImageChain(Document document, std::vector<Node> &nodes,
                               std::optional<NodeId> after = std::nullopt);
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
