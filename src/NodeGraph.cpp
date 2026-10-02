#include "NodeGraph.h"

#include "RenderPipeline.h"
#include "ofx/OfxHost.h"
#include "processors/OfxProcessor.h"
#include "processors/NativeExposureProcessor.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
#include <memory>

// Node creation currently happens on the UI thread. Sidecar V2 persists IDs
// across sessions; the counter only supplies fresh IDs for newly added nodes.
static unsigned long long gNextNodeId = 1;

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
    case ParameterType::Integer:
    case ParameterType::Choice: {
      const int *value = std::get_if<int>(&param.value);
      return std::to_string(value ? *value : 0);
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
    case ParameterType::Choice:
      processor.setParameterValue(param.id, (int)std::strtol(value.c_str(), nullptr, 10), false);
      return;
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

void applyColorDefaults(App &app, Node &node) {
  if (!node.processor) return;
  const auto params = node.processor->parameters();
  for (const ProcessorParameter &param : params) {
    if (param.type != ParameterType::Choice) continue;
    const char *wanted =
        param.label == "Input Color Space" ? colorSpaceName(app.inputSpace)
        : param.label == "Output Color Space" ? "sRGB"
                                                : nullptr;
    if (!wanted) continue;

    for (size_t i = 0; i < param.choices.size(); ++i) {
      if (param.choices[i] != wanted) continue;
      node.processor->setParameterValue(param.id, (int)i);
      break;
    }
  }
}

void syncOutputTag(App &app) {
  for (int n = (int)app.nodes.size() - 1; n >= 0; --n) {
    if (!app.nodes[n].processor) continue;
    for (const ProcessorParameter &param : app.nodes[n].processor->parameters()) {
      if (param.secret || param.type != ParameterType::Choice || param.label != "Output Color Space") continue;
      const int *index = std::get_if<int>(&param.value);
      if (!index || *index < 0 || *index >= (int)param.choices.size()) continue;
      for (int i = 0; i < 4; ++i) {
        if (param.choices[*index] == kOutputSpaces[i]) {
          app.outputIndex = i;
          return;
        }
      }
    }
  }
}

void destroyNode(App &app, int index) {
  if (index < 0 || index >= (int)app.nodes.size()) return;
  waitRenderIdle(app);
  app.nodes.erase(app.nodes.begin() + index);
  if (app.nodes.empty())
    app.selectedNode = -1;
  else if (app.selectedNode >= (int)app.nodes.size())
    app.selectedNode = (int)app.nodes.size() - 1;
  else if (app.selectedNode > index)
    --app.selectedNode;
  app.paramFilter[0] = '\0';
  scheduleRender(app);
}

void clearNodes(App &app) {
  waitRenderIdle(app);
  app.nodes.clear();
  app.selectedNode = -1;
  app.paramFilter[0] = '\0';
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
  applyColorDefaults(app, node);
  for (const ProcessorParameter &param : node.processor->parameters())
    if (param.type == ParameterType::Group) node.groupOpen[param.id] = param.groupInitiallyOpen;

  app.nodes.push_back(std::move(node));
  app.selectedNode = (int)app.nodes.size() - 1;
  app.paramFilter[0] = '\0';
  syncOutputTag(app);
  scheduleRender(app);
  return true;
}

bool addNode(App &app, int pluginIndex) {
  if (pluginIndex < 0 || pluginIndex >= (int)gPlugins.size()) return false;
  waitRenderIdle(app);

  auto processor = OfxProcessor::create(pluginIndex);
  if (!processor) {
    app.setStatus("Plugin failed to create an instance");
    return false;
  }
  return appendProcessorNode(app, std::move(processor));
}

bool addNativeExposureNode(App &app) {
  waitRenderIdle(app);
  return appendProcessorNode(app, std::make_unique<NativeExposureProcessor>());
}

void moveNode(App &app, int from, int to) {
  if (from < 0 || to < 0 || from >= (int)app.nodes.size() || to >= (int)app.nodes.size() || from == to) return;
  waitRenderIdle(app);
  Node node = std::move(app.nodes[from]);
  app.nodes.erase(app.nodes.begin() + from);
  app.nodes.insert(app.nodes.begin() + to, std::move(node));
  app.selectedNode = to;
  syncOutputTag(app);
  scheduleRender(app);
}

PersistChain captureChain(const App &app) {
  PersistChain chain;
  if (app.selectedNode >= 0 && app.selectedNode < (int)app.nodes.size())
    chain.selectedNodeId = app.nodes[app.selectedNode].id;

  for (const Node &node : app.nodes) {
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
        if (param.secret || !persistedParameterType(param.type)) continue;
        persisted.paramsJson[param.id] = paramValueJson(param);
      }
    } else {
      persisted.backend = node.storedBackend.empty() ? "unknown" : node.storedBackend;
      persisted.identifier = node.storedIdentifier;
      persisted.label = node.storedLabel;
    }

    chain.nodes.push_back(std::move(persisted));
  }
  return chain;
}

void applyChain(App &app, const PersistChain &chain) {
  clearNodes(app);

  for (const PersistNode &persisted : chain.nodes) {
    const std::string backend = persisted.backend.empty() ? "ofx" : persisted.backend;
    bool created = false;

    if (backend == "ofx") {
      const int pluginIndex = findPluginIndex(persisted.identifier, persisted.label);
      created = pluginIndex >= 0 && addNode(app, pluginIndex);
    } else if (backend == "native" && persisted.identifier == NativeExposureProcessor::kIdentifier) {
      created = addNativeExposureNode(app);
    }

    if (created) {
      Node &node = app.nodes.back();
      const int index = (int)app.nodes.size() - 1;
      if (!persisted.id.empty()) node.id = restoredNodeId(app, persisted.id, index);
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
          if (it != persisted.paramsJson.end()) applyParamValueJson(*node.processor, param, it->second);
        }
      }
      continue;
    }

    Node node;
    node.id = restoredNodeId(app, persisted.id);
    node.enabled = persisted.enabled;
    node.storedBackend = backend;
    node.storedIdentifier = persisted.identifier;
    node.storedLabel = persisted.label;
    node.preservedParamsJson = persisted.paramsJson;
    node.groupOpen = persisted.groupOpen;
    app.nodes.push_back(std::move(node));
  }

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

  syncOutputTag(app);
  scheduleRender(app);
}
