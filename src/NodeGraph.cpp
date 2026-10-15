#include "NodeGraph.h"

#include "DocumentMutation.h"
#include "GraphValidation.h"
#include "GraphCommands.h"
#include "persist/ProjectPersistPriv.h"
#include "ofx/OfxHost.h"
#include "processors/OfxProcessor.h"
#include "processors/NativeExposureProcessor.h"
#include "processors/NativeCstProcessor.h"
#include "processors/CtlProcessor.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
#include <memory>

// Node creation currently happens on the UI thread. Sidecar V2 persists IDs
// across sessions; the counter only supplies fresh IDs for newly added nodes.
static unsigned long long gNextNodeId = 1;

using document_detail::DocumentMutation;

// Mutation helpers borrow the public operation's scope; capture and parameter
// helpers only translate live state to/from persistence records.
static void clearNodesWithinMutation(App &app);
static bool appendProcessorNode(App &app, std::unique_ptr<Processor> processor);
static bool addOfxWithinMutation(App &app, int pluginIndex);
static bool addCtlWithinMutation(App &app, const std::string &path);
static Node makeProcessorNode(const std::vector<Node> &nodes, std::unique_ptr<Processor> processor);

static PersistNode capturePersistedNode(const Node &node, bool graphNative = false);
static std::vector<NodeId> linearCaptureOrder(const App &app);
static Node restoredNode(const std::vector<Node> &nodes, const PersistNode &persisted, bool preserveId, bool graphNative = false);
static bool parameterHasUnknownChoiceId(const Node &node, const ProcessorParameter &param);
static bool persistedParameterType(ParameterType type);
static std::string paramValueJson(const ProcessorParameter &param);
static std::string trimParamJson(std::string value);
static bool legacyNumericChoiceValue(const ProcessorParameter &param, const std::string &raw, int &selected);
static void applyParamValueJson(Processor &processor, const ProcessorParameter &param, const std::string &raw);

Node *findNode(App &app, const NodeId &id) {
  for (auto &node : app.nodes)
    if (node.id == id) return &node;
  return nullptr;
}

const Node *findNode(const App &app, const NodeId &id) {
  return findNode(GraphView{app.nodes, app.primarySource, app.documentOutput}, id);
}

const PortDeclaration *findOutput(const App &app, const OutputRef &output) {
  return findOutput(GraphView{app.nodes, app.primarySource, app.documentOutput}, output);
}

const std::optional<OutputRef> *findInput(const App &app, const InputAddress &input) {
  return findInput(GraphView{app.nodes, app.primarySource, app.documentOutput}, input);
}

std::string documentStateError(const App &app) {
  return graphStructureError({app.nodes, app.primarySource, app.documentOutput});
}

Node *selectedNode(App &app) {
  Node *node = app.selectedNodeId ? findNode(app, *app.selectedNodeId) : nullptr;
  return node && node->operation != NodeOperation::Source ? node : nullptr;
}

const Node *selectedNode(const App &app) {
  const Node *node = app.selectedNodeId ? findNode(app, *app.selectedNodeId) : nullptr;
  return node && node->operation != NodeOperation::Source ? node : nullptr;
}

bool selectNode(App &app, std::optional<NodeId> id) {
  if (id) {
    const Node *node = findNode(app, *id);
    if (!node || node->operation == NodeOperation::Source) return false;
  }
  if (app.selectedNodeId != id) {
    app.selectedNodeId = std::move(id);
    app.paramFilter[0] = '\0';
  }
  return true;
}

std::string nodeDisplayName(const Node &node) {
  if (node.operation == NodeOperation::Source) return "Source";
  if (node.operation != NodeOperation::ImageProcessor) return "Test graph operation";
  if (node.processor) return node.processor->displayName();
  if (!node.storedLabel.empty()) return node.storedLabel + " (Missing)";
  if (!node.storedIdentifier.empty()) return node.storedIdentifier + " (Missing)";
  return "Missing processor";
}

void clearNodes(App &app) {
  if (app.nodes.size() == 1) {
    selectNode(app, std::nullopt);
    app.renderer.resetInspection();
    return;
  }
  DocumentMutation mutation(app);
  clearNodesWithinMutation(app);
  mutation.changed();
}

bool addNode(App &app, int pluginIndex) {
  if (pluginIndex < 0 || pluginIndex >= (int)gPlugins.size()) return false;
  DocumentMutation mutation(app);
  const bool added = addOfxWithinMutation(app, pluginIndex);
  if (added) mutation.changed();
  return added;
}

bool addNativeExposureNode(App &app) {
  DocumentMutation mutation(app);
  const bool added = appendProcessorNode(app, std::make_unique<NativeExposureProcessor>());
  if (added) mutation.changed();
  return added;
}

bool addNativeCstNode(App &app) {
  DocumentMutation mutation(app);
  const bool added = appendProcessorNode(app, std::make_unique<NativeCstProcessor>());
  if (added) mutation.changed();
  return added;
}

bool addCtlNode(App &app, const std::string &path) {
  DocumentMutation mutation(app);
  const bool added = addCtlWithinMutation(app, path);
  if (added) mutation.changed();
  return added;
}

bool destroyNode(App &app, const NodeId &id) {
  const Node *node = findNode(app, id);
  if (!node || node->operation == NodeOperation::Source) {
    app.setStatus(node ? "Source cannot be removed" : "Node no longer exists");
    return false;
  }
  const auto *producer = findInput(app, {id, std::nullopt, ports::image});
  // Preserve ordinary Image splice semantics. Other/disconnected nodes use
  // generic deletion, which disconnects consumers and requires an explicit
  // replacement when deleting documentOutput. Never guess that replacement.
  const auto result = graph::isUnaryImageNode(*node) && producer && *producer
      ? graph::deleteImageNode(app, id) : graph::deleteNode(app, id);
  if (!result.ok()) app.setStatus("Could not remove node: " + result.error);
  return result.ok();
}

bool moveNode(App &app, const NodeId &from, const NodeId &to) {
  std::vector<NodeId> spine;
  const auto error = graph::imageSpine({app.nodes, app.primarySource, app.documentOutput}, spine);
  if (!error.empty()) { app.setStatus("Cannot reorder nodes: " + error); return false; }
  const auto a = std::find(spine.begin(), spine.end(), from), b = std::find(spine.begin(), spine.end(), to);
  if (a == spine.end() || b == spine.end()) {
    app.setStatus("Only nodes on the main Image spine can be reordered");
    return false;
  }
  if (from == to) return true;
  std::optional<NodeId> before = to;
  if (a < b) before = b + 1 == spine.end() ? std::nullopt : std::optional<NodeId>(*(b + 1));
  const auto result = graph::moveImageNode(app, from, before);
  if (!result.ok()) app.setStatus("Cannot reorder nodes: " + result.error);
  return result.ok();
}

void setNodeEnabled(App &app, const NodeId &id, bool enabled) {
  Node *node = findNode(app, id);
  if (!node || node->enabled == enabled || node->operation == NodeOperation::Source) return;
  DocumentMutation mutation(app);
  node->enabled = enabled;
  mutation.changed();
}

bool captureNode(const App &app, const NodeId &id, PersistNode &out) {
  const Node *node = findNode(app, id);
  if (!node || node->operation != NodeOperation::ImageProcessor) return false;
  try { (void)linearCaptureOrder(app); } catch (const std::runtime_error &) { return false; }
  out = capturePersistedNode(*node);
  return true;
}

static std::vector<NodeId> linearCaptureOrder(const App &app) {
  std::vector<NodeId> spine;
  const auto error = graph::imageSpine({app.nodes, app.primarySource, app.documentOutput}, spine);
  if (!error.empty() || spine.size() + 1 != app.nodes.size())
    throw std::runtime_error("Current graph cannot be captured by linear V2");
  for (const auto &id : spine)
    if (findNode(app, id)->operation != NodeOperation::ImageProcessor)
      throw std::runtime_error("Current graph contains an operation unsupported by linear V2");
  return spine;
}

PersistChain captureChain(const App &app) {
  const auto spine = linearCaptureOrder(app);
  PersistChain chain;
  if (const Node *node = selectedNode(app); node && node->operation == NodeOperation::ImageProcessor)
    chain.selectedNodeId = node->id.value;

  chain.nodes.reserve(app.nodes.size());
  for (const auto &id : spine) chain.nodes.push_back(capturePersistedNode(*findNode(app, id)));
  return chain;
}

bool appendPersistedNode(App &app, const PersistNode &persisted, std::optional<NodeId> insertAfter) {
  std::optional<InputAddress> before;
  if (insertAfter) {
    std::vector<NodeId> spine;
    const auto error = graph::imageSpine({app.nodes, app.primarySource, app.documentOutput}, spine);
    if (!error.empty()) { app.setStatus(error); return false; }
    const auto after = *insertAfter;
    auto next = after == app.primarySource ? spine.begin() : std::find(spine.begin(), spine.end(), after);
    if (after != app.primarySource && next == spine.end()) {
      app.setStatus("Insertion requires a node on the main Image spine");
      return false;
    }
    if (after != app.primarySource) ++next;
    if (next != spine.end()) before = InputAddress{*next, std::nullopt, ports::image};
  }
  DocumentMutation mutation(app);
  try {
    Node node = restoredNode(app.nodes, persisted, false);
    std::optional<NodeId> selection = node.id;
    const auto result = graph::insertImageNode(graph::Document{app.nodes, app.primarySource, app.documentOutput},
                                               std::move(node), before);
    if (!result.ok()) { app.setStatus(result.error); return false; }
    mutation.changed();
    selectNode(app, std::move(selection));
    return true;
  } catch (const std::exception &error) {
    app.setStatus(error.what());
    return false;
  }
}

void applyChain(App &app, const PersistChain &chain) {
  if (app.nodes.size() == 1 && chain.nodes.empty()) {
    selectNode(app, std::nullopt);
    app.renderer.resetInspection();
    return;
  }
  DocumentMutation mutation(app);
  auto restored = document_detail::restoreChain(app, chain);
  document_detail::replaceChain(app, std::move(restored));
  mutation.changed();
}

PersistGraph captureGraph(GraphView graph) {
  const auto error = graphStructureError(graph);
  if (!error.empty()) throw std::runtime_error(error);
  PersistGraph record;
  record.primarySource = graph.primarySource;
  record.documentOutput = graph.documentOutput;
  record.nodes.reserve(graph.nodes.size());
  for (const auto &node : graph.nodes) record.nodes.push_back(capturePersistedNode(node, true));
  const auto recordError = persist_detail::graphRecordError(record, true);
  if (!recordError.empty()) throw std::runtime_error(recordError);
  return record;
}

namespace document_detail {
RestoredGraph restoreGraph(const PersistGraph &graph) {
  const auto error = persist_detail::graphRecordError(graph, true);
  if (!error.empty()) throw std::runtime_error(error);
  RestoredGraph restored;
  restored.primarySource = graph.primarySource;
  restored.output = graph.documentOutput;
  restored.nodes.reserve(graph.nodes.size());
  // First restore identities and backend state. Bindings may refer forwards in
  // storage; no insertion/splice command may infer topology from this order.
  for (const auto &record : graph.nodes) {
    Node node = record.operation == NodeOperation::ImageProcessor
        ? restoredNode(restored.nodes, record, /*preserveId=*/true, /*graphNative=*/true) : Node(record.operation);
    node.id = NodeId{record.id};
    node.enabled = record.enabled;
    restored.nodes.push_back(std::move(node));
  }
  for (size_t i = 0; i < graph.nodes.size(); ++i) {
    auto &node = restored.nodes[i];
    const auto &record = graph.nodes[i];
    node.fixedInputs = record.inputs;
    node.slots = record.slots;
    if (!nodeInterface(node.operation).slotInputs.empty() && !node.restoreNextSlotId(record.nextSlotId))
      throw std::runtime_error("Invalid Slot allocator state");
  }
  const auto structuralError = graphStructureError({restored.nodes, restored.primarySource, restored.output});
  if (!structuralError.empty()) throw std::runtime_error(structuralError);
  return restored;
}

RestoredChain restoreChain(App &app, const PersistChain &chain) {
  RestoredChain restored;
  restored.nodes.reserve(chain.nodes.size() + 1);
  Node source(NodeOperation::Source);
  source.id = app.primarySource;
  restored.nodes.push_back(std::move(source));
  restored.output = {app.primarySource, ports::image};
  for (const PersistNode &persisted : chain.nodes) {
    Node node = restoredNode(restored.nodes, persisted, true);
    const auto result = graph::insertImageNode(graph::Document{restored.nodes, app.primarySource, restored.output},
                                               std::move(node));
    if (!result.ok()) throw std::runtime_error(result.error);
  }
  for (const Node &node : restored.nodes)
    if (node.operation == NodeOperation::ImageProcessor && node.id.value == chain.selectedNodeId)
      restored.selection = node.id;
  // Retain the existing ordinal/default restoration without extending V1/V2.
  if (!restored.selection && chain.selectedNode >= 0 && chain.selectedNode < (int)chain.nodes.size())
    restored.selection = restored.nodes[chain.selectedNode + 1].id;
  if (!restored.selection && restored.nodes.size() > 1) restored.selection = restored.nodes[1].id;
  return restored;
}

void replaceChain(App &app, RestoredChain &&chain) {
  app.nodes.swap(chain.nodes);
  std::swap(app.documentOutput, chain.output);
  // A replacement can reuse a selected NodeId for a different processor.
  selectNode(app, std::nullopt);
  selectNode(app, std::move(chain.selection));
  app.renderer.resetInspection();
}
}  // namespace document_detail

bool hasUnknownProcessorChoiceIds(const App &app) {
  for (const Node &node : app.nodes) {
    if (!node.processor) continue;
    for (const ProcessorParameter &param : node.processor->parameters())
      if (parameterHasUnknownChoiceId(node, param)) return true;
  }
  return false;
}

static bool nodeIdExists(const std::vector<Node> &nodes, const std::string &id) {
  if (id.empty()) return false;
  for (const Node &node : nodes)
    if (node.id.value == id) return true;
  return false;
}

static NodeId makeNodeId(const std::vector<Node> &nodes) {
  for (;;) {
    const std::string id = "node-" + std::to_string(gNextNodeId++);
    if (!nodeIdExists(nodes, id)) return NodeId{id};
  }
}

static NodeId restoredNodeId(const std::vector<Node> &nodes, const std::string &requested) {
  if (!requested.empty() && !nodeIdExists(nodes, requested)) return NodeId{requested};
  return makeNodeId(nodes);
}

static int findPluginIndex(const std::string &identifier, const std::string &labelFallback) {
  if (!identifier.empty()) {
    for (int i = 0; i < (int)gPlugins.size(); ++i) {
      OfxPlugin *p = gPlugins[i].plugin;
      if (p && p->pluginIdentifier && identifier == p->pluginIdentifier) return i;
    }
  }
  if (!labelFallback.empty()) {
    for (int i = 0; i < (int)gPlugins.size(); ++i)
      if (gPlugins[i].label == labelFallback) return i;
  }
  return -1;
}

static void clearNodesWithinMutation(App &app) {
  // Preserve the same Source operation/identity across grade and image changes.
  auto source = std::find_if(app.nodes.begin(), app.nodes.end(), [&](const Node &node) {
    return node.id == app.primarySource && node.operation == NodeOperation::Source;
  });
  assert(source != app.nodes.end() && "Document is missing its primary Source");
  if (source == app.nodes.end()) throw std::logic_error("Document is missing its primary Source");
  Node retainedSource = std::move(*source);
  app.nodes.clear();
  app.nodes.push_back(std::move(retainedSource));
  app.documentOutput = {app.primarySource, ports::image};
  selectNode(app, std::nullopt);
  app.renderer.resetInspection();
}

static Node makeProcessorNode(const std::vector<Node> &nodes, std::unique_ptr<Processor> processor) {
  Node node;
  node.id = makeNodeId(nodes);
  node.processor = std::move(processor);
  if (node.processor) {
    node.storedBackend = processorBackendName(node.processor->backend());
    node.storedIdentifier = node.processor->identifier();
    node.storedLabel = node.processor->displayName();
    for (const ProcessorParameter &param : node.processor->parameters())
      if (param.type == ParameterType::Group) node.groupOpen[param.id] = param.groupInitiallyOpen;
  }
  return node;
}
static bool appendProcessorNode(App &app, std::unique_ptr<Processor> processor) {
  if (!processor) return false;
  auto node = makeProcessorNode(app.nodes, std::move(processor));
  std::optional<NodeId> selection = node.id; // Allocate identity before committing the graph.
  const auto result = graph::insertImageNode(graph::Document{app.nodes, app.primarySource, app.documentOutput}, std::move(node));
  if (!result.ok()) { app.setStatus(result.error); return false; }
  selectNode(app, std::move(selection));
  return true;
}

static bool addOfxWithinMutation(App &app, int pluginIndex) {
  auto processor = OfxProcessor::create(pluginIndex);
  if (!processor) {
    app.setStatus("Plugin failed to create an instance");
    return false;
  }
  return appendProcessorNode(app, std::move(processor));
}

static bool addCtlWithinMutation(App &app, const std::string &path) {
  std::string error;
  auto processor = CtlProcessor::create(path, &error);
  if (!processor) {
    app.setStatus("Could not load CTL" + (error.empty() ? std::string() : ": " + error));
    return false;
  }
  return appendProcessorNode(app, std::move(processor));
}

static PersistNode capturePersistedNode(const Node &node, bool graphNative) {
  PersistNode persisted;
  persisted.id = node.id.value;
  persisted.enabled = node.enabled;
  if (!graphNative) persisted.groupOpen = node.groupOpen;
  if (graphNative) {
    persisted.operation = node.operation;
    persisted.inputs = node.fixedInputs;
    persisted.slots = node.slots;
    persisted.nextSlotId = node.nextSlotIdForPersistence();
    if (node.operation != NodeOperation::ImageProcessor) return persisted;
  }
  persisted.paramsJson = node.preservedParamsJson;

  if (node.processor) {
    persisted.backend = processorBackendName(node.processor->backend());
    persisted.identifier = node.processor->identifier();
    persisted.label = graphNative && !node.storedLabel.empty() ? node.storedLabel : node.processor->displayName();

    for (const ProcessorParameter &param : node.processor->parameters()) {
      if (param.secret || !param.persistValue || !persistedParameterType(param.type)) {
        if (graphNative) persisted.paramsJson.erase(param.id);
        continue;
      }

      if (graphNative) {
        const auto currentJson = paramValueJson(param);
        if (persist_detail::parameterValue(param, currentJson) != param.value)
          throw std::runtime_error("Unrepresentable parameter: " + param.id);
        const auto saved = node.preservedParamsJson.find(param.id);
        // Retain precise tokens when the current understood value is unchanged.
        // Unknown IDs are already merged above and never converted to doubles.
        if (saved != node.preservedParamsJson.end() &&
            persist_detail::parameterValue(param, saved->second) == param.value) continue;
        persisted.paramsJson[param.id] = currentJson;
        continue;
      }

      // If a newer build wrote a stable choice ID that this build does not
      // recognise, keep the opaque original value instead of replacing it
      // with this build's fallback/default selection on save or copy.
      if (param.type == ParameterType::Choice && !param.choiceIds.empty()) {
        auto preserved = node.preservedParamsJson.find(param.id);
        if (preserved != node.preservedParamsJson.end()) {
          std::string preservedId;
          if (parseJsonStringValue(trimParamJson(preserved->second), preservedId)) {
            if (std::find(param.choiceIds.begin(), param.choiceIds.end(), preservedId) == param.choiceIds.end())
              continue;
          } else {
            int legacySelected = 0;
            if (!legacyNumericChoiceValue(param, preserved->second, legacySelected))
              continue;
          }
        }
      }

      persisted.paramsJson[param.id] = paramValueJson(param);
    }
  } else {
    persisted.backend = node.storedBackend.empty() ? "unknown" : node.storedBackend;
    persisted.identifier = node.storedIdentifier;
    persisted.label = node.storedLabel;
  }

  return persisted;
}

static Node restoredNode(const std::vector<Node> &nodes, const PersistNode &persisted, bool preserveId, bool graphNative) {
  const std::string backend = persisted.backend.empty() ? "ofx" : persisted.backend;
  std::unique_ptr<Processor> processor;
  if (backend == "ofx") {
    const int index = findPluginIndex(persisted.identifier, graphNative ? std::string() : persisted.label);
    if (index >= 0) processor = OfxProcessor::create(index);
  } else if (backend == "native" && persisted.identifier == NativeExposureProcessor::kIdentifier) {
    processor = std::make_unique<NativeExposureProcessor>();
  } else if (backend == "native" && persisted.identifier == NativeCstProcessor::kIdentifier) {
    processor = std::make_unique<NativeCstProcessor>();
  } else if (backend == "ctl") {
    processor = CtlProcessor::create(persisted.identifier);
  }
  Node node = makeProcessorNode(nodes, std::move(processor));
  if (graphNative) node.id = NodeId{persisted.id};
  else if (preserveId) node.id = restoredNodeId(nodes, persisted.id);
  node.enabled = persisted.enabled;
  if (!graphNative) node.groupOpen = persisted.groupOpen;
  node.storedBackend = backend;
  node.storedIdentifier = persisted.identifier;
  node.storedLabel = persisted.label;
  node.preservedParamsJson = persisted.paramsJson;
  if (node.processor) {
    for (const auto &param : node.processor->parameters()) {
      const auto value = persisted.paramsJson.find(param.id);
      if (value == persisted.paramsJson.end()) continue;
      if (!graphNative) { applyParamValueJson(*node.processor, param, value->second); continue; }
      if (param.secret || !param.persistValue || !persistedParameterType(param.type))
        throw std::runtime_error("Non-persistent installed parameter: " + param.id);
      const auto parsed = persist_detail::parameterValue(param, value->second);
      if (!node.processor->setParameterValue(param.id, parsed, false))
        throw std::runtime_error("Processor refused parameter: " + param.id);
      bool matched = false;
      for (const auto &updated : node.processor->parameters())
        if (updated.id == param.id && updated.value == parsed) matched = true;
      if (!matched) throw std::runtime_error("Processor changed saved parameter value: " + param.id);
    }
  }
  return node;
}

static bool parameterHasUnknownChoiceId(const Node &node, const ProcessorParameter &param) {
  if (param.type != ParameterType::Choice || param.choiceIds.empty()) return false;
  const auto it = node.preservedParamsJson.find(param.id);
  if (it == node.preservedParamsJson.end()) return false;

  std::string id;
  if (parseJsonStringValue(trimParamJson(it->second), id))
    return std::find(param.choiceIds.begin(), param.choiceIds.end(), id) == param.choiceIds.end();

  // Development builds briefly wrote stable choices as numbers. Treat a
  // numeric value as migratable only when it maps exactly to a current choice.
  int legacySelected = 0;
  return !legacyNumericChoiceValue(param, it->second, legacySelected);
}

// Doubles are written with max_digits10 so strtod restores the exact value;
// std::to_string's fixed 6 decimals changed restored renders.
static std::ostringstream jsonNumberStream() {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out.precision(std::numeric_limits<double>::max_digits10);
  return out;
}

static bool persistedParameterType(ParameterType type) {
  return type != ParameterType::Group && type != ParameterType::Page &&
         type != ParameterType::PushButton && type != ParameterType::Unsupported;
}

static std::string paramValueJson(const ProcessorParameter &param) {
  switch (param.type) {
    case ParameterType::String:
    case ParameterType::Custom: {
      const auto *value = std::get_if<std::string>(&param.value);
      return value ? jsonStringValue(*value) : "\"\"";
    }
    case ParameterType::Boolean: {
      const bool *value = std::get_if<bool>(&param.value);
      return value && *value ? "true" : "false";
    }
    case ParameterType::Integer: {
      const int *value = std::get_if<int>(&param.value);
      return std::to_string(value ? *value : 0);
    }
    case ParameterType::Choice: {
      const int *value = std::get_if<int>(&param.value);
      const int selected = value ? *value : 0;
      if (param.choiceIds.size() == param.choices.size() && !param.choiceIds.empty()) {
        size_t index = (size_t)selected;
        if (param.choiceValues.size() == param.choices.size()) {
          index = param.choiceIds.size();
          for (size_t i = 0; i < param.choiceValues.size(); ++i)
            if (param.choiceValues[i] == selected) {
              index = i;
              break;
            }
        }
        if (index < param.choiceIds.size()) return jsonStringValue(param.choiceIds[index]);
      }
      return std::to_string(selected);
    }
    case ParameterType::Vector: {
      const auto *value = std::get_if<std::vector<double>>(&param.value);
      std::ostringstream out = jsonNumberStream();
      out << '[';
      if (value) {
        for (size_t i = 0; i < value->size(); ++i) {
          if (i) out << ',';
          out << (*value)[i];
        }
      }
      out << ']';
      return out.str();
    }
    case ParameterType::Double: {
      const double *value = std::get_if<double>(&param.value);
      std::ostringstream out = jsonNumberStream();
      out << (value ? *value : 0.0);
      return out.str();
    }
    default:
      return "null";
  }
}

static std::string trimParamJson(std::string value) {
  while (!value.empty() && std::isspace((unsigned char)value.front())) value.erase(value.begin());
  while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
  return value;
}

static bool legacyNumericChoiceValue(const ProcessorParameter &param,
                                     const std::string &raw, int &selected) {
  if (param.type != ParameterType::Choice || param.choiceIds.empty()) return false;
  const std::string value = trimParamJson(raw);
  if (value.empty()) return false;

  char *end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str()) return false;
  while (*end && std::isspace((unsigned char)*end)) ++end;
  if (*end) return false;

  if (param.choiceValues.size() == param.choices.size()) {
    for (int candidate : param.choiceValues) {
      if (candidate == parsed) {
        selected = candidate;
        return true;
      }
    }
    return false;
  }

  if (parsed < 0 || parsed >= (long)param.choices.size()) return false;
  selected = (int)parsed;
  return true;
}

static void applyParamValueJson(Processor &processor, const ProcessorParameter &param, const std::string &raw) {
  const std::string value = trimParamJson(raw);
  switch (param.type) {
    case ParameterType::String:
    case ParameterType::Custom: {
      std::string parsed;
      if (parseJsonStringValue(value, parsed)) processor.setParameterValue(param.id, parsed, false);
      return;
    }
    case ParameterType::Boolean:
      processor.setParameterValue(param.id, value == "true" || value == "1", false);
      return;
    case ParameterType::Integer:
      processor.setParameterValue(param.id, (int)std::strtol(value.c_str(), nullptr, 10), false);
      return;
    case ParameterType::Choice: {
      if (param.choiceIds.size() == param.choices.size() && !param.choiceIds.empty()) {
        std::string id;
        if (parseJsonStringValue(value, id)) {
          for (size_t i = 0; i < param.choiceIds.size(); ++i) {
            if (param.choiceIds[i] != id) continue;
            const int selected =
                param.choiceValues.size() == param.choices.size() ? param.choiceValues[i] : (int)i;
            processor.setParameterValue(param.id, selected, false);
            return;
          }
          return;
        }

        // Development builds briefly persisted stable choices numerically.
        // Accept only a numeric value that maps exactly to a current choice;
        // captureChain() will normalise it back to the stable string ID.
        int selected = 0;
        if (legacyNumericChoiceValue(param, value, selected))
          processor.setParameterValue(param.id, selected, false);
        return;
      }

      // Backends without stable IDs retain their historical numeric choice
      // persistence.
      processor.setParameterValue(param.id, (int)std::strtol(value.c_str(), nullptr, 10), false);
      return;
    }
    case ParameterType::Double:
      processor.setParameterValue(param.id, std::strtod(value.c_str(), nullptr), false);
      return;
    case ParameterType::Vector: {
      const auto *current = std::get_if<std::vector<double>>(&param.value);
      if (!current || value.empty() || value.front() != '[') return;
      std::vector<double> parsed = *current;
      size_t i = 1;
      for (size_t dim = 0; dim < parsed.size() && i < value.size(); ++dim) {
        while (i < value.size() && (std::isspace((unsigned char)value[i]) || value[i] == ',')) ++i;
        char *end = nullptr;
        parsed[dim] = std::strtod(value.c_str() + i, &end);
        if (!end || end == value.c_str() + i) return;
        i = (size_t)(end - value.c_str());
      }
      processor.setParameterValue(param.id, parsed, false);
      return;
    }
    default:
      return;
  }
}
