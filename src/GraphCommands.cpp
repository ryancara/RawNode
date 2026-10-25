#include "GraphCommands.h"

#include "DocumentMutation.h"
#include "GraphValidation.h"
#include "NodeGraph.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace graph_detail {

// An operation-local topology copy, never an executable/persistent graph. No
// processor is cloned, moved or destroyed until the validated commit. Original
// processor ownership is validated before this copy omits the payloads.
class StagedEdit {
 public:
  explicit StagedEdit(graph::Document document)
      : error(graphStructureError(document)), document_(document), output_(document.documentOutput) {
    if (!error.empty()) return;
    nodes_.reserve(document.nodes.size() + 1);
    for (const auto &node : document.nodes) nodes_.push_back(copyTopology(node));
  }

  GraphView view() const { return {nodes_, document_.primarySource, output_}; }
  Node *node(const NodeId &id) {
    for (auto &node : nodes_)
      if (node.id == id) return &node;
    return nullptr;
  }
  std::optional<OutputRef> *input(const InputAddress &address) {
    if (!findInputDeclaration(view(), address)) return nullptr;
    Node *owner = node(address.nodeId);
    auto &bindings = address.slotId ? owner->findSlot(*address.slotId)->inputs : owner->fixedInputs;
    const auto found = bindings.find(address.inputPortId);
    return found == bindings.end() ? nullptr : &found->second;
  }
  std::string add(const Node &added, bool validate = true) {
    if (added.id.value.empty()) return "Empty NodeId";
    if (node(added.id)) return "Duplicate NodeId";
    if (added.operation == NodeOperation::Source) return "Cannot add a second Source";
    if (added.operation != NodeOperation::ImageProcessor && added.processor)
      return "Graph-native node has an Image processor";
    // A malformed operation is diagnosed without constructing a staging node.
    try {
      nodes_.push_back(copyTopology(added));
    } catch (const std::invalid_argument &) {
      return "Unknown graph operation";
    }
    return validate ? graphStructureError(view()) : std::string();
  }
  void erase(const NodeId &id, const std::optional<OutputRef> &imageSubstitute = std::nullopt) {
    const auto disconnectConsumers = [&](InputBindings &bindings) {
      for (auto &entry : bindings) {
        if (!entry.second || entry.second->nodeId != id) continue;
        entry.second = entry.second->outputPortId == ports::image ? imageSubstitute : std::nullopt;
      }
    };
    for (auto &node : nodes_) {
      disconnectConsumers(node.fixedInputs);
      for (auto &slot : node.slots) disconnectConsumers(slot.inputs);
    }
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node &node) {
      return node.id == id;
    }), nodes_.end());
  }
  OutputRef &output() { return output_; }

  graph::CommandResult commit(const std::vector<Node *> &additions, std::optional<SlotId> addedSlot, App *owner) {
    auto invalid = graphStructureError(view());
    if (!invalid.empty()) return {invalid, std::nullopt};
    if (additions.empty() && unchanged()) return {{}, addedSlot};
    // Staging reads immutable topology on the control thread. Only a validated
    // change drains preview/export, before touching live storage or processors.
    std::optional<document_detail::DocumentMutation> mutation;
    if (owner) mutation.emplace(*owner);
    // All fallible allocation happens before modifying the live topology.
    // Node moves, binding/Slot swaps and output swaps below cannot throw.
    static_assert(std::is_nothrow_move_constructible_v<Node>);
    static_assert(std::is_nothrow_move_assignable_v<Node>);
    static_assert(std::is_nothrow_swappable_v<InputBindings>);
    static_assert(std::is_nothrow_swappable_v<std::vector<Slot>>);
    static_assert(std::is_nothrow_swappable_v<OutputRef>);
    document_.nodes.reserve(document_.nodes.size() + additions.size());
    for (auto &live : document_.nodes) {
      Node *staged = node(live.id);
      if (staged) swapTopology(live, *staged);
    }
    for (auto *addition : additions) {
      swapTopology(*addition, *node(addition->id));
      document_.nodes.push_back(std::move(*addition));
    }
    document_.nodes.erase(std::remove_if(document_.nodes.begin(), document_.nodes.end(), [&](const Node &live) {
      return !node(live.id);
    }), document_.nodes.end());
    std::swap(document_.documentOutput, output_);
    if (owner) {
      for (auto position = owner->nodePositions.begin(); position != owner->nodePositions.end();) {
        // Commit must not allocate a temporary NodeId after changing topology.
        if (std::none_of(nodes_.begin(), nodes_.end(), [&](const Node &node) { return node.id.value == position->first; }))
          position = owner->nodePositions.erase(position);
        else ++position;
      }
      if (owner->selectedNodeId() && !findNode(*owner, *owner->selectedNodeId())) selectNode(*owner, std::nullopt);
      mutation->changed();
      owner->history.recordCompletedEdit(*owner, &*mutation);
    }
    return {{}, addedSlot};
  }

  std::string error;

 private:
  // Compare node membership/document output and exactly the fields committed
  // by swapTopology: bindings, ordered Slots and their ID high-water mark.
  // Slot identity/order is semantic even when bindings/settings are identical.
  // Every future SlotSettings field must participate here; swapping the Slot
  // vector already commits those fields. Retained node operation/enabled/processor
  // are not changed by these swaps. Keep comparison and commit aligned when
  // extending this boundary for Graph View.
  bool unchanged() const {
    if (output_ != document_.documentOutput || nodes_.size() != document_.nodes.size()) return false;
    for (size_t i = 0; i < nodes_.size(); ++i) {
      const Node &staged = nodes_[i], &live = document_.nodes[i];
      if (staged.id != live.id || staged.fixedInputs != live.fixedInputs ||
          staged.nextSlotId_ != live.nextSlotId_ || staged.slots.size() != live.slots.size()) return false;
      for (size_t j = 0; j < staged.slots.size(); ++j) {
        const Slot &a = staged.slots[j], &b = live.slots[j];
        if (a.id != b.id || a.inputs != b.inputs || a.settings.enabled != b.settings.enabled) return false;
      }
    }
    return true;
  }

  static Node copyTopology(const Node &original) {
    Node copy(original.operation);
    copy.id = original.id;
    copy.enabled = original.enabled;
    copy.fixedInputs = original.fixedInputs;
    copy.slots = original.slots;
    copy.nextSlotId_ = original.nextSlotId_;
    // Observe restored IDs before *any* command, including deletion of the
    // highest restored Slot before the next allocation.
    copy.observeSlotIds();
    return copy;
  }
  static void swapTopology(Node &live, Node &staged) noexcept {
    live.fixedInputs.swap(staged.fixedInputs);
    live.slots.swap(staged.slots);
    std::swap(live.nextSlotId_, staged.nextSlotId_);
  }
  graph::Document document_;
  OutputRef output_;
  std::vector<Node> nodes_;
};

}  // namespace graph_detail

using graph_detail::StagedEdit;
using graph::CommandResult;

template <typename Change>
static CommandResult editGraph(graph::Document document, Change change, Node *addition = nullptr, App *owner = nullptr) {
  StagedEdit staged(document);
  if (!staged.error.empty()) return {staged.error, std::nullopt};
  auto result = change(staged);
  if (!result.ok()) return result;
  return staged.commit(addition ? std::vector<Node *>{addition} : std::vector<Node *>{}, result.addedSlot, owner);
}

template <typename Change>
static CommandResult editGraph(App &app, Change change, Node *addition = nullptr) {
  document_detail::DocumentMutation::assertControlThread(app);
  if (!requireEditableGraph(app)) return {"The saved grade was not loaded; graph editing is disabled", std::nullopt};
  return editGraph(graph::Document{app.nodes, app.primarySource, app.documentOutput}, change, addition, &app);
}

static std::string connectionError(GraphView graph, const OutputRef &producer, const InputAddress &destination);
static std::string imageOutputError(GraphView graph, const OutputRef &output);

namespace graph {

template <typename Target>
static CommandResult connectGraph(Target &document, const OutputRef &producer, const InputAddress &destination) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto error = connectionError(edit.view(), producer, destination);
    if (!error.empty()) return {error, std::nullopt};
    auto *binding = edit.input(destination);
    if (*binding) return {"Destination already has a producer; use reconnect", std::nullopt};
    *binding = producer;
    return {};
  });
}

template <typename Target>
static CommandResult reconnectGraph(Target &document, const OutputRef &producer, const InputAddress &destination) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto error = connectionError(edit.view(), producer, destination);
    if (!error.empty()) return {error, std::nullopt};
    *edit.input(destination) = producer;
    return {};
  });
}

template <typename Target>
static CommandResult disconnectGraph(Target &document, const InputAddress &destination) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto *binding = edit.input(destination);
    if (!binding) return {"Destination input does not exist", std::nullopt};
    binding->reset();
    return {};
  });
}

template <typename Target>
static CommandResult addNodeGraph(Target &document, Node &&node) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    return {edit.add(node), std::nullopt};
  }, &node);
}

template <typename Target>
static CommandResult deleteNodeGraph(Target &document, const NodeId &id, std::optional<OutputRef> replacementOutput) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    const Node *deleted = edit.node(id);
    if (!deleted) return {"Node does not exist", std::nullopt};
    if (deleted->operation == NodeOperation::Source) return {"Cannot delete Source", std::nullopt};
    if (replacementOutput) {
      auto error = imageOutputError(edit.view(), *replacementOutput);
      if (!error.empty()) return {error, std::nullopt};
    }
    if (edit.output().nodeId == id) {
      if (!replacementOutput) return {"Deleting document output requires an explicit replacement", std::nullopt};
      edit.output() = *replacementOutput;
    }
    edit.erase(id);
    return {};
  });
}

template <typename Target>
static CommandResult setDocumentOutputGraph(Target &document, const OutputRef &output) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto error = imageOutputError(edit.view(), output);
    if (!error.empty()) return {error, std::nullopt};
    edit.output() = output;
    return {};
  });
}

template <typename Target>
static CommandResult addSlotGraph(Target &document, const NodeId &id, SlotSettings settings) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    Node *node = edit.node(id);
    if (!node) return {"Node does not exist", std::nullopt};
    if (nodeInterface(node->operation).slotInputs.empty()) return {"Node has no repeated inputs", std::nullopt};
    try {
      Slot slot = node->makeSlot();
      const SlotId added = slot.id;
      slot.settings = settings;
      node->slots.push_back(std::move(slot));
      return {{}, added};
    } catch (const std::overflow_error &error) {
      return {error.what(), std::nullopt};
    }
  });
}

template <typename Target>
static CommandResult deleteSlotGraph(Target &document, const NodeId &id, SlotId slot) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    Node *node = edit.node(id);
    if (!node || !node->findSlot(slot)) return {"Slot does not exist", std::nullopt};
    node->slots.erase(std::find_if(node->slots.begin(), node->slots.end(), [&](const Slot &entry) {
      return entry.id == slot;
    }));
    return {};
  });
}

template <typename Target>
static CommandResult reorderSlotGraph(Target &document, const NodeId &id, SlotId slot, std::optional<SlotId> before) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    Node *node = edit.node(id);
    if (!node || !node->findSlot(slot) || (before && !node->findSlot(*before)))
      return {"Slot does not exist", std::nullopt};
    if (before == slot) return {};
    auto from = std::find_if(node->slots.begin(), node->slots.end(), [&](const Slot &entry) { return entry.id == slot; });
    Slot moved = std::move(*from);
    node->slots.erase(from);
    auto target = std::find_if(node->slots.begin(), node->slots.end(), [&](const Slot &entry) {
      return before && entry.id == *before;
    });
    node->slots.insert(target, std::move(moved));
    return {};
  });
}

static CommandResult stageImageNodeInsertion(StagedEdit &edit, const Node &node, std::optional<InputAddress> before) {
  auto error = edit.add(node);
  if (!error.empty()) return {error, std::nullopt};
  Node *inserted = edit.node(node.id);
  if (!isUnaryImageNode(*inserted)) return {"Insertion requires a unary Image node", std::nullopt};
  if (inserted->fixedInputs.at(ports::image)) return {"New node's Image input is already connected", std::nullopt};
  OutputRef producer = edit.output();
  if (before) {
    if (before->slotId || before->inputPortId != ports::image)
      return {"Splice destination must be a fixed Image input", std::nullopt};
    auto *binding = edit.input(*before);
    if (!binding || !*binding) return {"Splice destination needs an existing connection", std::nullopt};
    producer = **binding;
    error = connectionError(edit.view(), {node.id, ports::image}, *before);
    if (!error.empty()) return {error, std::nullopt};
    *binding = OutputRef{node.id, ports::image};
  } else {
    edit.output() = {node.id, ports::image};
  }
  inserted->fixedInputs.at(ports::image) = producer;
  return {};
}

template <typename Target>
static CommandResult insertImageNodeGraph(Target &document, Node &&node, std::optional<InputAddress> before) {
  return editGraph(document, [&](StagedEdit &edit) { return stageImageNodeInsertion(edit, node, before); }, &node);
}

CommandResult validateImageNodeInsertion(Document document, std::optional<InputAddress> before,
                                        std::optional<OutputRef> branch) {
  StagedEdit edit(document);
  if (!edit.error.empty()) return {edit.error, std::nullopt};
  // Backend-independent prospective ImageProcessor topology. The placeholder
  // exists only in staging; construction/identity allocation remain with callers.
  Node node;
  node.id = NodeId{"insertion"};
  while (edit.node(node.id)) node.id.value += '-';
  if (branch) {
    if (before) return {"Insertion cannot both splice and branch", std::nullopt};
    node.fixedInputs.at(ports::image) = *branch;
    return {edit.add(node), std::nullopt};
  }
  const auto result = stageImageNodeInsertion(edit, node, before);
  return result.ok() ? CommandResult{graphStructureError(edit.view()), std::nullopt} : result;
}

CommandResult insertImageChain(Document document, std::vector<Node> &nodes, std::optional<NodeId> after) {
  StagedEdit edit(document);
  if (!edit.error.empty()) return {edit.error, std::nullopt};
  std::vector<NodeId> spine;
  auto error = imageSpine(document, spine);
  if (!error.empty()) return {error, std::nullopt};
  auto successor = spine.end();
  if (after) {
    if (*after == document.primarySource) successor = spine.begin();
    else {
      successor = std::find(spine.begin(), spine.end(), *after);
      if (successor == spine.end()) return {"Insertion requires a current main Image spine node", std::nullopt};
      ++successor;
    }
  }
  if (nodes.empty()) return {};
  std::vector<Node *> additions;
  additions.reserve(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto &node = nodes[i];
    if (node.operation != NodeOperation::ImageProcessor || !isUnaryImageNode(node) || node.fixedInputs.size() != 1)
      return {"Insertion requires an ordinary linear ImageProcessor chain", std::nullopt};
    const auto &input = node.fixedInputs.at(ports::image);
    if ((i == 0 && input) || (i && input != std::optional<OutputRef>{{nodes[i-1].id, ports::image}}))
      return {"Incoming chain has incompatible connections", std::nullopt};
    // Forward/internal references are validated after the complete batch exists.
    error = edit.add(node, false);
    if (!error.empty()) return {error, std::nullopt};
  }
  const OutputRef predecessor = after ? OutputRef{*after, ports::image} : edit.output();
  edit.node(nodes.front().id)->fixedInputs.at(ports::image) = predecessor;
  const OutputRef tail{nodes.back().id, ports::image};
  if (successor == spine.end()) edit.output() = tail;
  else *edit.input({*successor, std::nullopt, ports::image}) = tail;
  for (auto &node : nodes) additions.push_back(&node);
  return edit.commit(additions, std::nullopt, nullptr);
}

template <typename Target>
static CommandResult deleteImageNodeGraph(Target &document, const NodeId &id) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    const Node *deleted = edit.node(id);
    if (!deleted || !isUnaryImageNode(*deleted)) return {"Deletion requires a unary Image node; Source cannot be deleted", std::nullopt};
    const auto producer = deleted->fixedInputs.at(ports::image);
    if (!producer) return {"Image splice requires a connected producer", std::nullopt};
    if (edit.output().nodeId == id) {
      if (edit.output().outputPortId != ports::image)
        return {"Cannot substitute a secondary document output", std::nullopt};
      edit.output() = *producer;
    }
    edit.erase(id, producer);
    return {};
  });
}

template <typename Target>
static CommandResult moveImageNodeGraph(Target &document, const NodeId &id, std::optional<NodeId> before) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    std::vector<NodeId> spine;
    auto error = imageSpine(edit.view(), spine);
    if (!error.empty()) return {error, std::nullopt};
    auto from = std::find(spine.begin(), spine.end(), id);
    if (from == spine.end() || (before && std::find(spine.begin(), spine.end(), *before) == spine.end()))
      return {"Move endpoints must be non-Source nodes on the main Image spine", std::nullopt};
    if (before == id) return {};
    spine.erase(from);
    const auto target = before ? std::find(spine.begin(), spine.end(), *before) : spine.end();
    spine.insert(target, id);
    OutputRef producer{edit.view().primarySource, ports::image};
    for (const auto &nodeId : spine) {
      edit.node(nodeId)->fixedInputs.at(ports::image) = producer;
      producer = {nodeId, ports::image};
    }
    edit.output() = producer;
    return {};
  });
}

CommandResult connect(Document document, const OutputRef &producer, const InputAddress &destination) {
  return connectGraph(document, producer, destination);
}
CommandResult reconnect(Document document, const OutputRef &producer, const InputAddress &destination) {
  return reconnectGraph(document, producer, destination);
}
CommandResult disconnect(Document document, const InputAddress &destination) {
  return disconnectGraph(document, destination);
}
CommandResult addNode(Document document, Node &&node) {
  return addNodeGraph(document, std::move(node));
}
CommandResult deleteNode(Document document, const NodeId &node, std::optional<OutputRef> replacementOutput) {
  return deleteNodeGraph(document, node, replacementOutput);
}
CommandResult setDocumentOutput(Document document, const OutputRef &output) {
  return setDocumentOutputGraph(document, output);
}
CommandResult addSlot(Document document, const NodeId &node, SlotSettings settings) {
  return addSlotGraph(document, node, settings);
}
CommandResult deleteSlot(Document document, const NodeId &node, SlotId slot) {
  return deleteSlotGraph(document, node, slot);
}
CommandResult reorderSlot(Document document, const NodeId &node, SlotId slot, std::optional<SlotId> before) {
  return reorderSlotGraph(document, node, slot, before);
}
CommandResult insertImageNode(Document document, Node &&node, std::optional<InputAddress> before) {
  return insertImageNodeGraph(document, std::move(node), before);
}
CommandResult deleteImageNode(Document document, const NodeId &node) {
  return deleteImageNodeGraph(document, node);
}
CommandResult moveImageNode(Document document, const NodeId &node, std::optional<NodeId> before) {
  return moveImageNodeGraph(document, node, before);
}

CommandResult connect(App &app, const OutputRef &producer, const InputAddress &destination) {
  return connectGraph(app, producer, destination);
}
CommandResult reconnect(App &app, const OutputRef &producer, const InputAddress &destination) {
  return reconnectGraph(app, producer, destination);
}
CommandResult disconnect(App &app, const InputAddress &destination) {
  return disconnectGraph(app, destination);
}
CommandResult addNode(App &app, Node &&node) {
  return addNodeGraph(app, std::move(node));
}
CommandResult deleteNode(App &app, const NodeId &node, std::optional<OutputRef> replacementOutput) {
  return deleteNodeGraph(app, node, replacementOutput);
}
CommandResult setDocumentOutput(App &app, const OutputRef &output) {
  return setDocumentOutputGraph(app, output);
}
CommandResult addSlot(App &app, const NodeId &node, SlotSettings settings) {
  return addSlotGraph(app, node, settings);
}
CommandResult deleteSlot(App &app, const NodeId &node, SlotId slot) {
  return deleteSlotGraph(app, node, slot);
}
CommandResult reorderSlot(App &app, const NodeId &node, SlotId slot, std::optional<SlotId> before) {
  return reorderSlotGraph(app, node, slot, before);
}
CommandResult insertImageNode(App &app, Node &&node, std::optional<InputAddress> before) {
  return insertImageNodeGraph(app, std::move(node), before);
}
CommandResult deleteImageNode(App &app, const NodeId &node) {
  return deleteImageNodeGraph(app, node);
}
CommandResult moveImageNode(App &app, const NodeId &node, std::optional<NodeId> before) {
  return moveImageNodeGraph(app, node, before);
}

}  // namespace graph

static std::string connectionError(GraphView graph, const OutputRef &producer, const InputAddress &destination) {
  if (!findNode(graph, producer.nodeId)) return "Producer node does not exist";
  const auto *output = findOutput(graph, producer);
  if (!output || output->direction != PortDirection::Output) return "Producer output port does not exist";
  if (!findNode(graph, destination.nodeId)) return "Destination node does not exist";
  const auto *input = findInputDeclaration(graph, destination);
  if (!input || input->direction != PortDirection::Input || !findInput(graph, destination))
    return "Destination input does not exist";
  if (input->type != output->type) return "Connection value type mismatch";
  return {};
}

bool graph::isUnaryImageNode(const Node &node) {
  if (node.operation == NodeOperation::Source || !node.slots.empty()) return false;
  const auto &interface = nodeInterface(node.operation);
  const auto *input = interface.findFixedPort(ports::image, PortDirection::Input);
  const auto *output = interface.findFixedPort(ports::image, PortDirection::Output);
  if (!input || !input->required || input->type != ValueType::Image || !output || output->type != ValueType::Image ||
      !interface.slotInputs.empty()) return false;
  for (const auto &port : interface.fixedPorts)
    if (port.direction == PortDirection::Input && port.required && port.id != ports::image) return false;
  return true;
}

static std::string imageOutputError(GraphView graph, const OutputRef &output) {
  const auto *port = findOutput(graph, output);
  if (!port || port->type != ValueType::Image) return "Document output must name an existing Image output";
  return {};
}

std::string graph::imageSpine(GraphView graph, std::vector<NodeId> &spine) {
  spine.clear();
  auto error = graphStructureError(graph);
  if (!error.empty()) return error;
  OutputRef current = graph.documentOutput;
  while (current.nodeId != graph.primarySource) {
    const Node *node = findNode(graph, current.nodeId);
    if (current.outputPortId != ports::image || !node || !isUnaryImageNode(*node)) {
      error = "Main Image spine must contain unary Image nodes";
      break;
    }
    spine.push_back(current.nodeId);
    const auto &producer = node->fixedInputs.at(ports::image);
    if (!producer) {
      error = "Main Image spine has a disconnected input";
      break;
    }
    current = *producer;
  }
  if (error.empty() && current.outputPortId != ports::image) error = "Main Image spine must end at Source.image";
  std::reverse(spine.begin(), spine.end());
  return error;
}
