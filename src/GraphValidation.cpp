#include "GraphValidation.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_map>

static std::string nodeStateError(const Node &node, const NodeId &primarySource);
using NodeLookup = std::unordered_map<std::string, const Node *>;
static std::string bindingError(const NodeLookup &nodes, const InputBindings &bindings,
                                const NodeInterface &interface, bool slotInputs);
static std::string dependencyError(GraphView graph, const NodeLookup &nodes);

std::string graphStructureError(GraphView graph) {
  NodeLookup nodes;
  nodes.reserve(graph.nodes.size());
  int sources = 0;
  for (const auto &node : graph.nodes) {
    if (node.id.value.empty()) return "Empty NodeId";
    if (!nodes.emplace(node.id.value, &node).second) return "Duplicate NodeId";
    if (node.operation == NodeOperation::Source) ++sources;
    const auto error = nodeStateError(node, graph.primarySource);
    if (!error.empty()) return error;
  }
  if (sources != 1) return "Document must contain exactly one primary Source";
  const auto producer = nodes.find(graph.documentOutput.nodeId.value);
  const auto *output = producer == nodes.end() ? nullptr :
      nodeInterface(producer->second->operation).findFixedPort(graph.documentOutput.outputPortId, PortDirection::Output);
  if (!output || output->type != ValueType::Image) return "Document output must name an existing Image output";
  for (const auto &node : graph.nodes) {
    const auto &interface = nodeInterface(node.operation);
    auto error = bindingError(nodes, node.fixedInputs, interface, false);
    if (!error.empty()) return error;
    for (const auto &slot : node.slots) {
      error = bindingError(nodes, slot.inputs, interface, true);
      if (!error.empty()) return error;
    }
  }
  return dependencyError(graph, nodes);
}

std::string requestedGraphError(GraphView graph, const OutputRef &requested) {
  // This gate bounds the recursive closure check below to the shared depth
  // limit. Required-input readiness still applies only to the requested closure.
  auto error = graphStructureError(graph);
  if (!error.empty()) return error;
  if (!findOutput(graph, requested)) return "Requested output does not exist";
  std::set<std::string> visited;
  std::function<std::string(const Node &)> visit = [&](const Node &node) -> std::string {
    if (!visited.insert(node.id.value).second) return {};
    const auto &interface = nodeInterface(node.operation);
    const auto check = [&](const InputBindings &bindings, bool slotInputs) -> std::string {
      for (const auto &entry : bindings) {
        const auto *port = slotInputs ? interface.findSlotInput(entry.first)
                                     : interface.findFixedPort(entry.first, PortDirection::Input);
        if (!entry.second) {
          if (port->required) return "Missing required input: " + node.id.value + "." + port->id.value;
          continue;
        }
        auto upstream = visit(*findNode(graph, entry.second->nodeId));
        if (!upstream.empty()) return upstream;
      }
      return {};
    };
    auto missing = check(node.fixedInputs, false);
    if (!missing.empty()) return missing;
    for (const auto &slot : node.slots) {
      missing = check(slot.inputs, true);
      if (!missing.empty()) return missing;
    }
    return {};
  };
  return visit(*findNode(graph, requested.nodeId));
}

static std::string nodeStateError(const Node &node, const NodeId &primarySource) {
  if (node.operation == NodeOperation::Source) {
    if (node.id != primarySource) return "Source is not the primary Source";
    if (!node.enabled || node.processor || !node.fixedInputs.empty() || !node.slots.empty())
      return "Invalid Source state";
  } else if (node.operation != NodeOperation::ImageProcessor && node.processor) {
    return "Graph-native node has an Image processor";
  }
  const double strength = node.strength;
  if (node.operation == NodeOperation::ImageProcessor &&
      (!std::isfinite(strength) || strength < 0 || strength > 1))
    return "Invalid node Strength (expected 0..1)";
  const NodeInterface *interface = nullptr;
  try {
    interface = &nodeInterface(node.operation);
  } catch (const std::invalid_argument &) {
    return "Unknown graph operation";
  }
  size_t fixedInputCount = 0;
  for (const auto &port : interface->fixedPorts)
    if (port.direction == PortDirection::Input) {
      ++fixedInputCount;
      if (node.fixedInputs.count(port.id) != 1) return "Missing fixed input storage";
    }
  if (fixedInputCount != node.fixedInputs.size()) return "Unknown fixed input storage";
  if (!node.slots.empty() && interface->slotInputs.empty()) return "Node has no repeated inputs";
  std::set<uint64_t> slotIds;
  for (const auto &slot : node.slots) {
    if (slot.id.value == 0) return "Invalid SlotId";
    if (!slotIds.insert(slot.id.value).second) return "Duplicate SlotId";
    if (slot.inputs.size() != interface->slotInputs.size()) return "Invalid Slot input storage";
    for (const auto &port : interface->slotInputs)
      if (port.direction != PortDirection::Input || slot.inputs.count(port.id) != 1)
        return "Missing Slot input storage";
  }
  return {};
}

static std::string bindingError(const NodeLookup &nodes, const InputBindings &bindings,
                                const NodeInterface &interface, bool slotInputs) {
  for (const auto &entry : bindings) {
    if (!entry.second) continue;
    const auto producer = nodes.find(entry.second->nodeId.value);
    if (producer == nodes.end()) return "Connected producer node does not exist";
    const auto *output = nodeInterface(producer->second->operation).findFixedPort(entry.second->outputPortId, PortDirection::Output);
    if (!output) return "Connected output port does not exist";
    const auto *input = slotInputs ? interface.findSlotInput(entry.first)
                                  : interface.findFixedPort(entry.first, PortDirection::Input);
    if (!input || input->direction != PortDirection::Input || output->direction != PortDirection::Output)
      return "Connection direction mismatch";
    if (input->type != output->type) return "Connection value type mismatch";
  }
  return {};
}

static std::string dependencyError(GraphView graph, const NodeLookup &nodes) {
  struct State {
    bool complete = false;
    size_t depth = 1;
  };
  struct Frame {
    const Node *node;
    size_t bindings = 0; // Fixed inputs, then each Slot in stored order.
    InputBindings::const_iterator next;
    size_t depth = 1;
    explicit Frame(const Node &node) : node(&node), next(node.fixedInputs.begin()) {}
  };
  std::unordered_map<const Node *, State> states;
  states.reserve(nodes.size());
  std::vector<Frame> stack;
  stack.reserve(kMaxGraphDependencyDepth);
  const auto tooDeep = [](const Node &node) {
    return "Graph dependency depth exceeds supported limit of " +
        std::to_string(kMaxGraphDependencyDepth) + " nodes at " + node.id.value;
  };
  for (const auto &root : graph.nodes) {
    if (states.count(&root)) continue;
    states.emplace(&root, State{});
    stack.emplace_back(root);
    while (!stack.empty()) {
      auto &frame = stack.back();
      const auto &bindings = frame.bindings == 0 ? frame.node->fixedInputs :
          frame.node->slots[frame.bindings - 1].inputs;
      if (frame.next == bindings.end()) {
        if (frame.bindings < frame.node->slots.size()) {
          frame.next = frame.node->slots[frame.bindings++].inputs.begin();
        } else {
          states.at(frame.node) = {true, frame.depth};
          stack.pop_back();
        }
        continue;
      }
      if (!frame.next->second) { ++frame.next; continue; }
      const Node *upstream = nodes.at(frame.next->second->nodeId.value);
      const auto found = states.find(upstream);
      if (found == states.end()) {
        // Explicit frames make even malicious chains safe. Stop early rather
        // than allocating a frame for every node of an unsupported chain.
        if (stack.size() == kMaxGraphDependencyDepth) return tooDeep(*upstream);
        states.emplace(upstream, State{});
        stack.emplace_back(*upstream);
        continue; // Revisit this binding with the completed upstream depth.
      }
      if (!found->second.complete) return "Graph contains a cycle";
      frame.depth = std::max(frame.depth, found->second.depth + 1);
      if (frame.depth > kMaxGraphDependencyDepth) return tooDeep(*frame.node);
      ++frame.next;
    }
  }
  return {};
}
