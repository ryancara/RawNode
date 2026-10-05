#include "NodeGraph.h"

#include "DocumentMutation.h"
#include "ofx/OfxHost.h"
#include "processors/OfxProcessor.h"
#include "processors/NativeExposureProcessor.h"
#include "processors/NativeCstProcessor.h"
#include "processors/CtlProcessor.h"

#include <algorithm>
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

static bool nodeIdExists(const App &app, const std::string &id, int skipIndex = -1) {
  if (id.empty()) return false;
  for (int i = 0; i < (int)app.nodes.size(); ++i) {
    if (i == skipIndex) continue;
    if (app.nodes[i].id == id) return true;
  }
  return false;
}

static std::string makeNodeId(const App &app) {
  for (;;) {
    const std::string id = "node-" + std::to_string(gNextNodeId++);
    if (!nodeIdExists(app, id)) return id;
  }
}

static std::string restoredNodeId(const App &app, const std::string &requested, int skipIndex = -1) {
  if (!requested.empty() && !nodeIdExists(app, requested, skipIndex)) return requested;
  return makeNodeId(app);
}

Node *selectedNode(App &app) {
  if (app.selectedNode < 0 || app.selectedNode >= (int)app.nodes.size()) return nullptr;
  return &app.nodes[app.selectedNode];
}

std::string nodeDisplayName(const Node &node) {
  if (node.processor) return node.processor->displayName();
  if (!node.storedLabel.empty()) return node.storedLabel + " (Missing)";
  if (!node.storedIdentifier.empty()) return node.storedIdentifier + " (Missing)";
  return "Missing processor";
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

void destroyNode(App &app, int index) {
  if (index < 0 || index >= (int)app.nodes.size()) return;
  DocumentMutation mutation(app);
  app.nodes.erase(app.nodes.begin() + index);
  if (app.nodes.empty())
    app.selectedNode = -1;
  else if (app.selectedNode >= (int)app.nodes.size())
    app.selectedNode = (int)app.nodes.size() - 1;
  else if (app.selectedNode > index)
    --app.selectedNode;
  app.paramFilter[0] = '\0';

  mutation.changed();
}

static void clearNodesWithinMutation(App &app) {
  app.nodes.clear();
  app.selectedNode = -1;
  app.paramFilter[0] = '\0';
}

void clearNodes(App &app) {
  if (app.nodes.empty()) {
    app.selectedNode = -1;
    app.paramFilter[0] = '\0';
    return;
  }
  DocumentMutation mutation(app);
  clearNodesWithinMutation(app);
  mutation.changed();
}

static bool appendProcessorNode(App &app, std::unique_ptr<Processor> processor) {
  if (!processor) return false;

  Node node;
  node.id = makeNodeId(app);
  node.processor = std::move(processor);
  node.storedBackend = processorBackendName(node.processor->backend());
  node.storedIdentifier = node.processor->identifier();
  node.storedLabel = node.processor->displayName();

  if (app.preview.w) node.processor->setRenderSize(app.preview.w, app.preview.h);
  for (const ProcessorParameter &param : node.processor->parameters())
    if (param.type == ParameterType::Group) node.groupOpen[param.id] = param.groupInitiallyOpen;

  app.nodes.push_back(std::move(node));
  app.selectedNode = (int)app.nodes.size() - 1;
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

static bool addCtlWithinMutation(App &app, const std::string &path) {
  std::string error;
  auto processor = CtlProcessor::create(path, &error);
  if (!processor) {
    app.setStatus("Could not load CTL" + (error.empty() ? std::string() : ": " + error));
    return false;
  }
  return appendProcessorNode(app, std::move(processor));
}

bool addCtlNode(App &app, const std::string &path) {
  DocumentMutation mutation(app);
  const bool added = addCtlWithinMutation(app, path);
  if (added) mutation.changed();
  return added;
}

static void moveNodeWithinMutation(App &app, int from, int to) {
  Node node = std::move(app.nodes[from]);
  app.nodes.erase(app.nodes.begin() + from);
  app.nodes.insert(app.nodes.begin() + to, std::move(node));
  app.selectedNode = to;
}

void moveNode(App &app, int from, int to) {
  if (from < 0 || to < 0 || from >= (int)app.nodes.size() || to >= (int)app.nodes.size() || from == to) return;
  DocumentMutation mutation(app);
  moveNodeWithinMutation(app, from, to);
  mutation.changed();
}

void setNodeEnabled(App &app, int index, bool enabled) {
  if (index < 0 || index >= (int)app.nodes.size() || app.nodes[index].enabled == enabled) return;
  DocumentMutation mutation(app);
  app.nodes[index].enabled = enabled;
  mutation.changed();
}

static PersistNode capturePersistedNode(const Node &node) {
  PersistNode persisted;
  persisted.id = node.id;
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

bool captureNode(const App &app, int index, PersistNode &out) {
  if (index < 0 || index >= (int)app.nodes.size()) return false;
  out = capturePersistedNode(app.nodes[index]);
  return true;
}

PersistChain captureChain(const App &app) {
  PersistChain chain;
  if (app.selectedNode >= 0 && app.selectedNode < (int)app.nodes.size())
    chain.selectedNodeId = app.nodes[app.selectedNode].id;

  chain.nodes.reserve(app.nodes.size());
  for (const Node &node : app.nodes)
    chain.nodes.push_back(capturePersistedNode(node));
  return chain;
}

static void appendRestoredWithinMutation(App &app, const PersistNode &persisted,
                                        bool preserveId) {
  const std::string backend = persisted.backend.empty() ? "ofx" : persisted.backend;
  bool created = false;

  if (backend == "ofx") {
    const int pluginIndex = findPluginIndex(persisted.identifier, persisted.label);
    created = pluginIndex >= 0 && addOfxWithinMutation(app, pluginIndex);
  } else if (backend == "native" && persisted.identifier == NativeExposureProcessor::kIdentifier) {
    created = appendProcessorNode(app, std::make_unique<NativeExposureProcessor>());
  } else if (backend == "native" && persisted.identifier == NativeCstProcessor::kIdentifier) {
    created = appendProcessorNode(app, std::make_unique<NativeCstProcessor>());
  } else if (backend == "ctl") {
    created = addCtlWithinMutation(app, persisted.identifier);
  }

  if (created) {
    Node &node = app.nodes.back();
    if (preserveId && !persisted.id.empty())
      node.id = restoredNodeId(app, persisted.id, (int)app.nodes.size() - 1);
    node.enabled = persisted.enabled;
    node.groupOpen = persisted.groupOpen;
    node.storedBackend = backend;
    node.storedIdentifier = persisted.identifier;
    node.storedLabel = persisted.label;
    node.preservedParamsJson = persisted.paramsJson;

    if (node.processor) {
      const auto params = node.processor->parameters();
      for (const ProcessorParameter &param : params) {
        auto it = persisted.paramsJson.find(param.id);
        if (it != persisted.paramsJson.end())
          applyParamValueJson(*node.processor, param, it->second);
      }
    }

    return;
  }

  // Unavailable processors are pasted as the same non-destructive placeholder
  // used by Sidecar V2 restore. A copied node is still useful even if another
  // machine does not currently have its plugin/script installed.
  Node node;
  node.id = preserveId ? restoredNodeId(app, persisted.id) : makeNodeId(app);
  node.enabled = persisted.enabled;
  node.storedBackend = backend;
  node.storedIdentifier = persisted.identifier;
  node.storedLabel = persisted.label;
  node.preservedParamsJson = persisted.paramsJson;
  node.groupOpen = persisted.groupOpen;
  app.nodes.push_back(std::move(node));
}

bool appendPersistedNode(App &app, const PersistNode &persisted, int insertAfter) {
  DocumentMutation mutation(app);
  mutation.changed();  // Restoration can throw after appending a node.
  const int oldCount = (int)app.nodes.size();
  const int targetIndex =
      insertAfter >= 0 && insertAfter < oldCount ? insertAfter + 1 : oldCount;
  appendRestoredWithinMutation(app, persisted, false);
  if (targetIndex != oldCount)
    moveNodeWithinMutation(app, oldCount, targetIndex);
  app.selectedNode = targetIndex;
  app.paramFilter[0] = '\0';
  return true;
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

bool hasUnknownProcessorChoiceIds(const App &app) {
  for (const Node &node : app.nodes) {
    if (!node.processor) continue;
    for (const ProcessorParameter &param : node.processor->parameters())
      if (parameterHasUnknownChoiceId(node, param)) return true;
  }
  return false;
}

void applyChain(App &app, const PersistChain &chain) {
  if (app.nodes.empty() && chain.nodes.empty()) {
    app.selectedNode = -1;
    app.paramFilter[0] = '\0';
    return;
  }
  DocumentMutation mutation(app);
  mutation.changed();  // Mark replacement before any fallible restoration work.
  clearNodesWithinMutation(app);

  for (const PersistNode &persisted : chain.nodes)
    appendRestoredWithinMutation(app, persisted, true);

  app.selectedNode = -1;
  if (!chain.selectedNodeId.empty()) {
    for (int i = 0; i < (int)app.nodes.size(); ++i) {
      if (app.nodes[i].id == chain.selectedNodeId) {
        app.selectedNode = i;
        break;
      }
    }
  }

  // V1 migration fallback.
  if (app.selectedNode < 0 && chain.selectedNode >= 0 && chain.selectedNode < (int)app.nodes.size())
    app.selectedNode = chain.selectedNode;
  if (app.selectedNode < 0 && !app.nodes.empty()) app.selectedNode = 0;
}
