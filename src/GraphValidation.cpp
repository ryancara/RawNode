#include "GraphValidation.h"

#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_map>

static std::string nodeStateError(const Node &node, const NodeId &primarySource);
static std::string bindingError(GraphView graph, const InputBindings &bindings,
                                const NodeInterface &interface, bool slotInputs);
static std::string cycleError(GraphView graph);

std::string graphStructureError(GraphView graph) {
  std::set<std::string> nodeIds;
  int sources = 0;
  for (const auto &node : graph.nodes) {
    if (node.id.value.empty()) return "Empty NodeId";
    if (!nodeIds.insert(node.id.value).second) return "Duplicate NodeId";
    if (node.operation == NodeOperation::Source) ++sources;
    const auto error = nodeStateError(node, graph.primarySource);
    if (!error.empty()) return error;
  }
  if (sources != 1) return "Document must contain exactly one primary Source";
  const auto *output = findOutput(graph, graph.documentOutput);
  if (!output || output->type != ValueType::Image) return "Document output must name an existing Image output";
  for (const auto &node : graph.nodes) {
    const auto &interface = nodeInterface(node.operation);
    auto error = bindingError(graph, node.fixedInputs, interface, false);
    if (!error.empty()) return error;
    for (const auto &slot : node.slots) {
      error = bindingError(graph, slot.inputs, interface, true);
      if (!error.empty()) return error;
    }
  }
  return cycleError(graph);
}

std::string requestedGraphError(GraphView graph, const OutputRef &requested) {
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

static std::string bindingError(GraphView graph, const InputBindings &bindings,
                                const NodeInterface &interface, bool slotInputs) {
  for (const auto &entry : bindings) {
    if (!entry.second) continue;
    if (!findNode(graph, entry.second->nodeId)) return "Connected producer node does not exist";
    const auto *output = findOutput(graph, *entry.second);
    if (!output) return "Connected output port does not exist";
    const auto *input = slotInputs ? interface.findSlotInput(entry.first)
                                  : interface.findFixedPort(entry.first, PortDirection::Input);
    if (!input || input->direction != PortDirection::Input || output->direction != PortDirection::Output)
      return "Connection direction mismatch";
    if (input->type != output->type) return "Connection value type mismatch";
  }
  return {};
}

static std::string cycleError(GraphView graph) {
  // Derived marks only; all dependencies still live on the consumers. Disabled,
  // missing and optional nodes/inputs participate exactly like other edges.
  enum class Mark { Visiting, Complete };
  std::unordered_map<std::string, Mark> marks;
  std::function<bool(const Node &)> visit = [&](const Node &node) {
    const auto found = marks.find(node.id.value);
    if (found != marks.end()) return found->second == Mark::Visiting;
    marks.emplace(node.id.value, Mark::Visiting);
    const auto cyclic = [&](const InputBindings &bindings) {
      for (const auto &entry : bindings)
        if (entry.second && visit(*findNode(graph, entry.second->nodeId))) return true;
      return false;
    };
    if (cyclic(node.fixedInputs)) return true;
    for (const auto &slot : node.slots)
      if (cyclic(slot.inputs)) return true;
    marks.at(node.id.value) = Mark::Complete;
    return false;
  };
  for (const auto &node : graph.nodes)
    if (visit(node)) return "Graph contains a cycle";
  return {};
}
