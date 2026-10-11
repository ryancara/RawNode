#include "GraphCommands.h"

#include "DocumentMutation.h"
#include "GraphValidation.h"

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
  std::string add(const Node &added) {
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
    return graphStructureError(view());
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

  graph::CommandResult commit(Node *addition, std::optional<SlotId> addedSlot) {
    auto invalid = graphStructureError(view());
    if (!invalid.empty()) return {invalid, std::nullopt};
    // All fallible allocation happens before modifying the live topology.
    // Node moves, binding/Slot swaps and output swaps below cannot throw.
    static_assert(std::is_nothrow_move_constructible_v<Node>);
    static_assert(std::is_nothrow_move_assignable_v<Node>);
    static_assert(std::is_nothrow_swappable_v<InputBindings>);
    static_assert(std::is_nothrow_swappable_v<std::vector<Slot>>);
    static_assert(std::is_nothrow_swappable_v<OutputRef>);
    if (addition) document_.nodes.reserve(document_.nodes.size() + 1);
    for (auto &live : document_.nodes) {
      Node *staged = node(live.id);
      if (staged) swapTopology(live, *staged);
    }
    if (addition) {
      swapTopology(*addition, *node(addition->id));
      document_.nodes.push_back(std::move(*addition));
    }
    document_.nodes.erase(std::remove_if(document_.nodes.begin(), document_.nodes.end(), [&](const Node &live) {
      return !node(live.id);
    }), document_.nodes.end());
    std::swap(document_.documentOutput, output_);
    return {{}, addedSlot};
  }

  std::string error;

 private:
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
static CommandResult editGraph(graph::Document document, Change change, Node *addition = nullptr) {
  StagedEdit staged(document);
  if (!staged.error.empty()) return {staged.error, std::nullopt};
  auto result = change(staged);
  if (!result.ok()) return result;
  return staged.commit(addition, result.addedSlot);
}

static std::string connectionError(GraphView graph, const OutputRef &producer, const InputAddress &destination);
static bool unaryImageNode(const Node &node);
static std::string imageOutputError(GraphView graph, const OutputRef &output);
static std::string imageSpine(GraphView graph, std::vector<NodeId> &spine);

namespace graph {

CommandResult connect(Document document, const OutputRef &producer, const InputAddress &destination) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto error = connectionError(edit.view(), producer, destination);
    if (!error.empty()) return {error, std::nullopt};
    auto *binding = edit.input(destination);
    if (*binding) return {"Destination already has a producer; use reconnect", std::nullopt};
    *binding = producer;
    return {};
  });
}

CommandResult reconnect(Document document, const OutputRef &producer, const InputAddress &destination) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto error = connectionError(edit.view(), producer, destination);
    if (!error.empty()) return {error, std::nullopt};
    *edit.input(destination) = producer;
    return {};
  });
}

CommandResult disconnect(Document document, const InputAddress &destination) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto *binding = edit.input(destination);
    if (!binding) return {"Destination input does not exist", std::nullopt};
    binding->reset();
    return {};
  });
}

CommandResult addNode(Document document, Node &&node) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    return {edit.add(node), std::nullopt};
  }, &node);
}

CommandResult deleteNode(Document document, const NodeId &id, std::optional<OutputRef> replacementOutput) {
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

CommandResult setDocumentOutput(Document document, const OutputRef &output) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto error = imageOutputError(edit.view(), output);
    if (!error.empty()) return {error, std::nullopt};
    edit.output() = output;
    return {};
  });
}

CommandResult addSlot(Document document, const NodeId &id, SlotSettings settings) {
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

CommandResult deleteSlot(Document document, const NodeId &id, SlotId slot) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    Node *node = edit.node(id);
    if (!node || !node->findSlot(slot)) return {"Slot does not exist", std::nullopt};
    node->slots.erase(std::find_if(node->slots.begin(), node->slots.end(), [&](const Slot &entry) {
      return entry.id == slot;
    }));
    return {};
  });
}

CommandResult reorderSlot(Document document, const NodeId &id, SlotId slot, std::optional<SlotId> before) {
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

CommandResult insertImageNode(Document document, Node &&node, std::optional<InputAddress> before) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    auto error = edit.add(node);
    if (!error.empty()) return {error, std::nullopt};
    Node *inserted = edit.node(node.id);
    if (!unaryImageNode(*inserted)) return {"Insertion requires a unary Image node", std::nullopt};
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
  }, &node);
}

CommandResult deleteImageNode(Document document, const NodeId &id) {
  return editGraph(document, [&](StagedEdit &edit) -> CommandResult {
    const Node *deleted = edit.node(id);
    if (!deleted || !unaryImageNode(*deleted)) return {"Deletion requires a unary Image node; Source cannot be deleted", std::nullopt};
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

CommandResult moveImageNode(Document document, const NodeId &id, std::optional<NodeId> before) {
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
    OutputRef producer{document.primarySource, ports::image};
    for (const auto &nodeId : spine) {
      edit.node(nodeId)->fixedInputs.at(ports::image) = producer;
      producer = {nodeId, ports::image};
    }
    edit.output() = producer;
    return {};
  });
}

// Live-document plumbing is deliberately separate from graph edit meaning.
// The control thread owns all writes; DocumentMutation drains preview/export
// before either processor ownership or consumer-owned topology can change.
template <typename Change>
static CommandResult mutateApp(App &app, Change change) {
  document_detail::DocumentMutation mutation(app);
  std::optional<NodeId> selected;
  if (app.selectedNode >= 0 && app.selectedNode < static_cast<int>(app.nodes.size()))
    selected = app.nodes[app.selectedNode].id;
  auto result = change(Document{app.nodes, app.primarySource, app.documentOutput});
  if (result.ok()) {
    if (selected) {
      auto found = std::find_if(app.nodes.begin(), app.nodes.end(), [&](const Node &node) { return node.id == *selected; });
      app.selectedNode = found == app.nodes.end() ? -1 : static_cast<int>(found - app.nodes.begin());
      if (app.selectedNode < 0) app.paramFilter[0] = '\0';
    }
    mutation.changed();
  }
  return result;
}

CommandResult connect(App &app, const OutputRef &producer, const InputAddress &destination) {
  return mutateApp(app, [&](Document document) { return connect(document, producer, destination); });
}
CommandResult reconnect(App &app, const OutputRef &producer, const InputAddress &destination) {
  return mutateApp(app, [&](Document document) { return reconnect(document, producer, destination); });
}
CommandResult disconnect(App &app, const InputAddress &destination) {
  return mutateApp(app, [&](Document document) { return disconnect(document, destination); });
}
CommandResult addNode(App &app, Node &&node) {
  return mutateApp(app, [&](Document document) { return addNode(document, std::move(node)); });
}
CommandResult deleteNode(App &app, const NodeId &node, std::optional<OutputRef> replacementOutput) {
  return mutateApp(app, [&](Document document) { return deleteNode(document, node, replacementOutput); });
}
CommandResult setDocumentOutput(App &app, const OutputRef &output) {
  return mutateApp(app, [&](Document document) { return setDocumentOutput(document, output); });
}
CommandResult addSlot(App &app, const NodeId &node, SlotSettings settings) {
  return mutateApp(app, [&](Document document) { return addSlot(document, node, settings); });
}
CommandResult deleteSlot(App &app, const NodeId &node, SlotId slot) {
  return mutateApp(app, [&](Document document) { return deleteSlot(document, node, slot); });
}
CommandResult reorderSlot(App &app, const NodeId &node, SlotId slot, std::optional<SlotId> before) {
  return mutateApp(app, [&](Document document) { return reorderSlot(document, node, slot, before); });
}
CommandResult insertImageNode(App &app, Node &&node, std::optional<InputAddress> before) {
  return mutateApp(app, [&](Document document) { return insertImageNode(document, std::move(node), before); });
}
CommandResult deleteImageNode(App &app, const NodeId &node) {
  return mutateApp(app, [&](Document document) { return deleteImageNode(document, node); });
}
CommandResult moveImageNode(App &app, const NodeId &node, std::optional<NodeId> before) {
  return mutateApp(app, [&](Document document) { return moveImageNode(document, node, before); });
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

static bool unaryImageNode(const Node &node) {
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

static std::string imageSpine(GraphView graph, std::vector<NodeId> &spine) {
  OutputRef current = graph.documentOutput;
  while (current.nodeId != graph.primarySource) {
    const Node *node = findNode(graph, current.nodeId);
    if (current.outputPortId != ports::image || !node || !unaryImageNode(*node))
      return "Main Image spine must contain unary Image nodes";
    spine.push_back(current.nodeId);
    const auto &producer = node->fixedInputs.at(ports::image);
    if (!producer) return "Main Image spine has a disconnected input";
    current = *producer;
  }
  if (current.outputPortId != ports::image) return "Main Image spine must end at Source.image";
  std::reverse(spine.begin(), spine.end());
  return {};
}
