#include "NodeGraph.h"

#include "DocumentMutation.h"
#include "GraphValidation.h"
#include "GraphCommands.h"
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
static Node makeProcessorNode(App &app, std::unique_ptr<Processor> processor);

static PersistNode capturePersistedNode(const Node &node);
static std::vector<NodeId> linearCaptureOrder(const App &app);
static NodeId appendRestoredWithinMutation(App &app, const PersistNode &persisted, bool preserveId,
                                          std::optional<InputAddress> before = std::nullopt);
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
    app.paramFilter[0] = '\0';
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
  NodeId added = appendRestoredWithinMutation(app, persisted, false, before);
  mutation.changed();
  selectNode(app, std::move(added));
  app.paramFilter[0] = '\0';
  return true;
}

void applyChain(App &app, const PersistChain &chain) {
  if (app.nodes.size() == 1 && chain.nodes.empty()) {
    selectNode(app, std::nullopt);
    app.paramFilter[0] = '\0';
    app.renderer.resetInspection();
    return;
  }
  DocumentMutation mutation(app);
  mutation.changed();  // Mark replacement before any fallible restoration work.
  clearNodesWithinMutation(app);

  for (const PersistNode &persisted : chain.nodes)
    appendRestoredWithinMutation(app, persisted, true);

  selectNode(app, std::nullopt);
  if (!chain.selectedNodeId.empty()) {
    for (int i = 0; i < (int)app.nodes.size(); ++i) {
      if (app.nodes[i].operation == NodeOperation::ImageProcessor && app.nodes[i].id.value == chain.selectedNodeId) {
        selectNode(app, app.nodes[i].id);
        break;
      }
    }
  }

  // V1 migration fallback.
  // Source is omitted from historical V1/V2 chains and kept at the front on restore.
  if (!app.selectedNodeId && chain.selectedNode >= 0 && chain.selectedNode < (int)chain.nodes.size())
    selectNode(app, app.nodes[chain.selectedNode + 1].id);
  if (!app.selectedNodeId && app.hasProcessingOutput()) selectNode(app, app.nodes[1].id);
}

bool hasUnknownProcessorChoiceIds(const App &app) {
  for (const Node &node : app.nodes) {
    if (!node.processor) continue;
    for (const ProcessorParameter &param : node.processor->parameters())
      if (parameterHasUnknownChoiceId(node, param)) return true;
  }
  return false;
}

static bool nodeIdExists(const App &app, const std::string &id, int skipIndex = -1) {
  if (id.empty()) return false;
  for (int i = 0; i < (int)app.nodes.size(); ++i) {
    if (i == skipIndex) continue;
    if (app.nodes[i].id.value == id) return true;
  }
  return false;
}

static NodeId makeNodeId(const App &app) {
  for (;;) {
    const std::string id = "node-" + std::to_string(gNextNodeId++);
    if (!nodeIdExists(app, id)) return NodeId{id};
  }
}

static NodeId restoredNodeId(const App &app, const std::string &requested, int skipIndex = -1) {
  if (!requested.empty() && !nodeIdExists(app, requested, skipIndex)) return NodeId{requested};
  return makeNodeId(app);
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
  app.paramFilter[0] = '\0';
  app.renderer.resetInspection();
}

static Node makeProcessorNode(App &app, std::unique_ptr<Processor> processor) {
  Node node;
  node.id = makeNodeId(app);
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
  auto node = makeProcessorNode(app, std::move(processor));
  std::optional<NodeId> selection = node.id; // Allocate identity before committing the graph.
  const auto result = graph::insertImageNode(graph::Document{app.nodes, app.primarySource, app.documentOutput}, std::move(node));
  if (!result.ok()) { app.setStatus(result.error); return false; }
  selectNode(app, std::move(selection));
  app.paramFilter[0] = '\0';
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

static PersistNode capturePersistedNode(const Node &node) {
  PersistNode persisted;
  persisted.id = node.id.value;
  persisted.enabled = node.enabled;
  persisted.groupOpen = node.groupOpen;
  persisted.paramsJson = node.preservedParamsJson;

  if (node.processor) {
    persisted.backend = processorBackendName(node.processor->backend());
    persisted.identifier = node.processor->identifier();
    persisted.label = node.processor->displayName();

    for (const ProcessorParameter &param : node.processor->parameters()) {
      if (param.secret || !param.persistValue || !persistedParameterType(param.type)) continue;

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

static NodeId appendRestoredWithinMutation(App &app, const PersistNode &persisted,
                                          bool preserveId, std::optional<InputAddress> before) {
  const std::string backend = persisted.backend.empty() ? "ofx" : persisted.backend;
  std::unique_ptr<Processor> processor;
  if (backend == "ofx") {
    const int index = findPluginIndex(persisted.identifier, persisted.label);
    if (index >= 0) processor = OfxProcessor::create(index);
  } else if (backend == "native" && persisted.identifier == NativeExposureProcessor::kIdentifier) {
    processor = std::make_unique<NativeExposureProcessor>();
  } else if (backend == "native" && persisted.identifier == NativeCstProcessor::kIdentifier) {
    processor = std::make_unique<NativeCstProcessor>();
  } else if (backend == "ctl") {
    processor = CtlProcessor::create(persisted.identifier);
  }
  Node node = makeProcessorNode(app, std::move(processor));
  if (preserveId) node.id = restoredNodeId(app, persisted.id);
  node.enabled = persisted.enabled;
  node.groupOpen = persisted.groupOpen;
  node.storedBackend = backend;
  node.storedIdentifier = persisted.identifier;
  node.storedLabel = persisted.label;
  node.preservedParamsJson = persisted.paramsJson;
  if (node.processor) {
    for (const auto &param : node.processor->parameters()) {
      const auto value = persisted.paramsJson.find(param.id);
      if (value != persisted.paramsJson.end()) applyParamValueJson(*node.processor, param, value->second);
    }
  }
  // Identity/parameters are final before the splice; no post-commit ID rewrite
  // can leave documentOutput or consumer references pointing at a temporary ID.
  NodeId added = node.id;
  const auto result = graph::insertImageNode(graph::Document{app.nodes, app.primarySource, app.documentOutput},
                                              std::move(node), before);
  if (!result.ok()) throw std::runtime_error(result.error);
  return added;
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
