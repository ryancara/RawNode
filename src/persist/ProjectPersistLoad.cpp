#include "persist/ProjectPersist.h"
#include "persist/ProjectPersistPriv.h"
#include "persist/Json.h"
#include "GraphValidation.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdexcept>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Historical formats retain their literal-control string compatibility, but
// all lexical parsing uses the same reader as the graph-native codec.
bool parseJsonStringValue(const std::string &raw, std::string &out) {
  persist_json::Value value;
  std::string error;
  if (!persist_json::read(raw, value, error, true) || value.type != persist_json::Type::String) return false;
  out = value.string;
  return true;
}

static bool extractValueField(const std::string &json, const char *key, std::string &raw) {
  persist_json::Value object;
  std::string error;
  if (!persist_json::read(json, object, error, true) || object.type != persist_json::Type::Object) return false;
  const auto found = object.object.find(key);
  if (found == object.object.end()) return false;
  raw = found->second.raw;
  return true;
}

bool extractObject(const std::string &json, const char *key, std::string &objOut) {
  std::string raw;
  if (!extractValueField(json, key, raw) || raw.empty() || raw.front() != '{') return false;
  objOut = std::move(raw);
  return true;
}

bool extractArray(const std::string &json, const char *key, std::string &arrOut) {
  std::string raw;
  if (!extractValueField(json, key, raw) || raw.empty() || raw.front() != '[') return false;
  arrOut = std::move(raw);
  return true;
}

bool extractStringField(const std::string &json, const char *key, std::string &out) {
  std::string raw;
  return extractValueField(json, key, raw) && parseJsonStringValue(raw, out);
}

bool extractIntField(const std::string &json, const char *key, int &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  char *end = nullptr;
  long value = std::strtol(raw.c_str(), &end, 10);
  if (end == raw.c_str()) return false;
  while (*end && std::isspace((unsigned char)*end)) ++end;
  if (*end) return false;
  out = (int)value;
  return true;
}

bool extractFloatField(const std::string &json, const char *key, float &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  char *end = nullptr;
  double value = std::strtod(raw.c_str(), &end);
  if (end == raw.c_str()) return false;
  while (*end && std::isspace((unsigned char)*end)) ++end;
  if (*end) return false;
  out = (float)value;
  return true;
}

bool extractBoolField(const std::string &json, const char *key, bool &out) {
  std::string raw;
  if (!extractValueField(json, key, raw)) return false;
  if (raw == "true") {
    out = true;
    return true;
  }
  if (raw == "false") {
    out = false;
    return true;
  }
  return false;
}

void loadGuiFromJson(const std::string &guiObj, PersistGui &g) {
  extractIntField(guiObj, "outputIndex", g.legacyOutputIndex);
  extractStringField(guiObj, "outputColorSpace", g.outputColorSpace);
  extractStringField(guiObj, "outputGamma", g.outputGamma);
  extractStringField(guiObj, "rawDefaultColorSpace", g.rawDefaultColorSpace);
  extractStringField(guiObj, "rawDefaultGamma", g.rawDefaultGamma);
  extractIntField(guiObj, "exportFormat", g.exportFormat);
  extractIntField(guiObj, "jpegQuality", g.jpegQuality);
  extractIntField(guiObj, "previewRes", g.previewRes);
  extractIntField(guiObj, "themeIndex", g.themeIndex);
  extractBoolField(guiObj, "showLeft", g.showLeft);
  extractBoolField(guiObj, "showRight", g.showRight);
  extractFloatField(guiObj, "leftW", g.leftW);
  extractFloatField(guiObj, "rightW", g.rightW);
  extractBoolField(guiObj, "showFilmstrip", g.showFilmstrip);
  extractFloatField(guiObj, "filmstripH", g.filmstripH);
}

// Returns false on malformed input so the caller rejects the whole sidecar
// (and write-protects it) instead of silently restoring a partial node.
static bool parseParamsObject(const std::string &paramsObj, std::map<std::string, std::string> &params) {
  persist_json::Value object;
  std::string error;
  if (!persist_json::read(paramsObj, object, error, true) || object.type != persist_json::Type::Object) return false;
  for (const auto &entry : object.object) params[entry.first] = entry.second.raw;
  return true;
}

static void parseGroupOpen(const std::string &json, std::map<std::string, bool> &groupOpen) {
  persist_json::Value object;
  std::string error;
  if (!persist_json::read(json, object, error, true) || object.type != persist_json::Type::Object) return;
  for (const auto &entry : object.object)
    if (entry.second.type == persist_json::Type::Boolean) groupOpen[entry.first] = entry.second.raw == "true";
}

static bool parseNodeArray(const std::string &json, PersistChain &chain, bool v2) {
  persist_json::Value array;
  std::string error;
  if (!persist_json::read(json, array, error, true) || array.type != persist_json::Type::Array) return false;
  chain.nodes.clear();
  for (const auto &value : array.array) {
    if (value.type != persist_json::Type::Object) return false;
    const std::string &nodeObj = value.raw;
    PersistNode node;
    if (v2) {
      extractStringField(nodeObj, "id", node.id);
      extractStringField(nodeObj, "backend", node.backend);
      extractStringField(nodeObj, "identifier", node.identifier);
      extractStringField(nodeObj, "label", node.label);
      extractBoolField(nodeObj, "enabled", node.enabled);
      std::string uiObj, groupObj;
      if (extractObject(nodeObj, "ui", uiObj) && extractObject(uiObj, "groupOpen", groupObj))
        parseGroupOpen(groupObj, node.groupOpen);
    } else {
      node.backend = "ofx";
      extractStringField(nodeObj, "pluginIdentifier", node.identifier);
      extractStringField(nodeObj, "pluginLabel", node.label);
      extractBoolField(nodeObj, "enabled", node.enabled);
      std::string groupObj;
      if (extractObject(nodeObj, "groupOpen", groupObj)) parseGroupOpen(groupObj, node.groupOpen);
    }
    std::string paramsObj;
    if (extractValueField(nodeObj, "params", paramsObj) && !parseParamsObject(paramsObj, node.paramsJson)) return false;
    chain.nodes.push_back(std::move(node));
  }
  return true;
}

bool loadChainFromJson(const std::string &chainObj, PersistChain &chain) {
  extractIntField(chainObj, "selectedNode", chain.selectedNode);
  std::string nodesArr;
  if (!extractArray(chainObj, "nodes", nodesArr)) return false;
  return parseNodeArray(nodesArr, chain, false);
}

static bool loadGraphV2FromJson(const std::string &graphObj, PersistChain &chain) {
  extractStringField(graphObj, "selectedNodeId", chain.selectedNodeId);
  std::string nodesArr;
  if (!extractArray(graphObj, "nodes", nodesArr)) return false;
  return parseNodeArray(nodesArr, chain, true);
}

bool readAllText(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return true;
}

static bool parseGraphEnvelope(const std::string &json, const char *expectedFormat,
                               std::string &kind, PersistChain &chain,
                               PersistGradeColor *color) {
  std::string format;
  int version = 0;
  if (!extractStringField(json, "format", format) || format != expectedFormat ||
      !extractIntField(json, "version", version) || version != 1 ||
      !extractStringField(json, "kind", kind) ||
      (kind != "node" && kind != "grade"))
    return false;

  if (color) {
    *color = PersistGradeColor{};
    if (kind == "grade") {
      std::string colorObj;
      if (extractObject(json, "color", colorObj)) {
        extractStringField(colorObj, "rawColorSpace", color->rawColorSpace);
        extractStringField(colorObj, "rawGamma", color->rawGamma);
        extractStringField(colorObj, "outputColorSpace", color->outputColorSpace);
        extractStringField(colorObj, "outputGamma", color->outputGamma);
      }
    }
  }

  std::string graphObj;
  if (!extractObject(json, "graph", graphObj)) return false;
  return loadGraphV2FromJson(graphObj, chain);
}

bool parseTransferPayload(const std::string &json, std::string &kind, PersistChain &chain,
                          PersistGradeColor *color) {
  return parseGraphEnvelope(json, "rawnode-transfer", kind, chain, color);
}

bool loadPresetFile(const std::string &path, std::string &kind, PersistChain &chain,
                    PersistGradeColor *color) {
  std::string json;
  if (!readAllText(path, json)) return false;
  return parseGraphEnvelope(json, "rawnode-preset", kind, chain, color);
}

bool loadWorkspaceProject(const std::string &workspaceDir, PersistGui &gui, std::string &activeImageRel) {
  std::string json;
  if (!readAllText(workspaceProjectPath(workspaceDir), json)) return false;
  std::string guiObj;
  if (extractObject(json, "gui", guiObj)) loadGuiFromJson(guiObj, gui);
  extractStringField(json, "activeImage", activeImageRel);
  return true;
}

bool loadSidecarFile(const std::string &path, PersistSidecar &out) {
  std::string json;
  if (!readAllText(path, json)) return false;

  out = PersistSidecar{};
  extractStringField(json, "format", out.format);
  extractIntField(json, "version", out.version);
  extractStringField(json, "kind", out.kind);

  // Never rewrite a future RawNode schema as V2. The caller can use the
  // parsed version to explain why loading was refused.
  if (out.format == "rawnode-sidecar" && out.version != 2) return false;

  std::string guiObj;
  if (extractObject(json, "gui", guiObj)) loadGuiFromJson(guiObj, out.gui);

  if (out.format == "rawnode-sidecar") {
    extractStringField(json, "source", out.sourcePath);
    std::string rawObj;
    if (extractObject(json, "raw", rawObj)) {
      extractStringField(rawObj, "workingSpace", out.legacyRawWorkingSpace);
      extractStringField(rawObj, "colorSpace", out.rawColorSpace);
      extractStringField(rawObj, "gamma", out.rawGamma);
    }
    std::string graphObj;
    if (!extractObject(json, "graph", graphObj)) return false;
    return loadGraphV2FromJson(graphObj, out.chain);
  }

  // V1 ofxrawhost sidecars are accepted and upgraded to V2 when next saved.
  extractStringField(json, "sourcePath", out.sourcePath);
  std::string chainObj;
  if (!extractObject(json, "chain", chainObj)) return false;
  return loadChainFromJson(chainObj, out.chain);
}

// Graph-native schema. Every structural object is closed; only parameter
// values are opaque. Parsing never creates processors or changes a document.
namespace {
using persist_json::Type;
using persist_json::Value;

void expect(const Value &value, Type type, const std::string &path) {
  if (value.type != type) throw std::runtime_error(path + ": incorrect field type");
}
const Value &field(const Value &object, const char *key) {
  expect(object, Type::Object, key);
  const auto found = object.object.find(key);
  if (found == object.object.end()) throw std::runtime_error(std::string("Missing required field: ") + key);
  return found->second;
}
void fields(const Value &object, std::initializer_list<const char *> allowed, const char *path) {
  expect(object, Type::Object, path);
  for (const auto &entry : object.object) {
    bool known = false;
    for (const auto *key : allowed) if (entry.first == key) known = true;
    if (!known) throw std::runtime_error(std::string(path) + ": unknown field " + entry.first);
  }
}
std::string textField(const Value &object, const char *key, bool nonempty = false) {
  const auto &value = field(object, key);
  expect(value, Type::String, key);
  if (nonempty && value.string.empty()) throw std::runtime_error(std::string(key) + ": empty identifier");
  return value.string;
}
bool boolField(const Value &object, const char *key) {
  const auto &value = field(object, key);
  expect(value, Type::Boolean, key);
  return value.raw == "true";
}
OutputRef outputRef(const Value &value) {
  fields(value, {"node", "port"}, "endpoint");
  return {NodeId{textField(value, "node", true)}, PortId{textField(value, "port", true)}};
}
InputBindings inputBindings(const Value &value, const NodeInterface &interface, bool slots) {
  expect(value, Type::Object, "inputs");
  InputBindings bindings;
  const auto &ports = slots ? interface.slotInputs : interface.fixedPorts;
  for (const auto &port : ports) {
    if (port.direction != PortDirection::Input) continue;
    const auto found = value.object.find(port.id.value);
    if (found == value.object.end()) {
      if (port.required) throw std::runtime_error("Missing required input storage: " + port.id.value);
      bindings.emplace(port.id, std::nullopt);
    } else {
      bindings.emplace(port.id, found->second.type == Type::Null ? std::nullopt
                                     : std::optional<OutputRef>(outputRef(found->second)));
    }
  }
  for (const auto &entry : value.object)
    if (!bindings.count(PortId{entry.first})) throw std::runtime_error("Unknown input: " + entry.first);
  return bindings;
}
uint64_t slotNumber(const Value &value) {
  expect(value, Type::String, "Slot identity/allocation");
  const auto &text = value.string;
  if (text.empty() || text.front() == '0') throw std::runtime_error("Invalid Slot decimal string");
  uint64_t number = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9' || number > (UINT64_MAX - (digit - '0')) / 10)
      throw std::runtime_error("Invalid or overflowing Slot decimal string");
    number = number * 10 + digit - '0';
  }
  return number;
}
PersistNode nodeRecord(const Value &value, bool testOperations) {
  PersistNode node;
  node.id = textField(value, "id", true);
  node.operation = persist_detail::operationFromName(textField(value, "kind"), testOperations);
  if (node.operation == NodeOperation::Source) {
    fields(value, {"id", "kind"}, "source");
    return node;
  }
  const auto &interface = nodeInterface(node.operation);
  const bool slotted = !interface.slotInputs.empty();
  if (node.operation == NodeOperation::ImageProcessor)
    fields(value, {"id", "kind", "backend", "identifier", "label", "enabled", "params", "inputs"}, "processor");
  else if (slotted)
    fields(value, {"id", "kind", "enabled", "inputs", "slots", "nextSlotId"}, "test operation");
  else
    fields(value, {"id", "kind", "enabled", "inputs"}, "test operation");
  node.enabled = boolField(value, "enabled");
  node.inputs = inputBindings(field(value, "inputs"), interface, false);
  if (node.operation == NodeOperation::ImageProcessor) {
    node.backend = textField(value, "backend", true);
    node.identifier = textField(value, "identifier", true);
    if (value.object.count("label")) node.label = textField(value, "label");
    const auto &params = field(value, "params");
    expect(params, Type::Object, "params");
    for (const auto &entry : params.object) node.paramsJson.emplace(entry.first, entry.second.raw);
  }
  if (slotted) {
    node.nextSlotId = slotNumber(field(value, "nextSlotId"));
    const auto &slots = field(value, "slots");
    expect(slots, Type::Array, "slots");
    for (const auto &saved : slots.array) {
      fields(saved, {"id", "settings", "inputs"}, "slot");
      Slot slot{SlotId{slotNumber(field(saved, "id"))}, {}, {}};
      slot.inputs = inputBindings(field(saved, "inputs"), interface, true);
      const auto &settings = field(saved, "settings");
      fields(settings, {"enabled"}, "slot settings");
      if (settings.object.count("enabled")) slot.settings.enabled = boolField(settings, "enabled");
      node.slots.push_back(std::move(slot));
    }
  }
  return node;
}
void colorPair(const Value &value, std::string &gamut, std::string &gamma) {
  fields(value, {"colorSpace", "gamma"}, "color pair");
  gamut = textField(value, "colorSpace");
  gamma = textField(value, "gamma");
  ColorEncoding encoding;
  if (!colorEncodingFromIds(gamut, gamma, encoding)) throw std::runtime_error("Unknown colour identifier");
}
PersistGrade gradeRecord(const Value &value, bool testOperations, std::vector<std::string> &warnings) {
  fields(value, {"color", "graph", "positions"}, "grade");
  PersistGrade grade;
  const auto &color = field(value, "color");
  fields(color, {"rawWorking", "output"}, "color");
  colorPair(field(color, "rawWorking"), grade.color.rawColorSpace, grade.color.rawGamma);
  colorPair(field(color, "output"), grade.color.outputColorSpace, grade.color.outputGamma);
  const auto &graph = field(value, "graph");
  fields(graph, {"primarySource", "documentOutput", "nodes"}, "graph");
  grade.graph.primarySource = NodeId{textField(graph, "primarySource", true)};
  grade.graph.documentOutput = outputRef(field(graph, "documentOutput"));
  const auto &nodes = field(graph, "nodes");
  expect(nodes, Type::Array, "nodes");
  for (const auto &node : nodes.array) grade.graph.nodes.push_back(nodeRecord(node, testOperations));
  const auto error = persist_detail::graphRecordError(grade.graph, testOperations);
  if (!error.empty()) throw std::runtime_error(error);
  const auto positions = value.object.find("positions");
  if (positions != value.object.end()) {
    expect(positions->second, Type::Object, "positions");
    for (const auto &entry : positions->second.object) {
      fields(entry.second, {"x", "y"}, "position");
      PersistPosition position;
      if (!persist_json::finiteNumber(field(entry.second, "x"), position.x) ||
          !persist_json::finiteNumber(field(entry.second, "y"), position.y))
        throw std::runtime_error("Position requires finite numeric x/y: " + entry.first);
      bool exists = false;
      for (const auto &node : grade.graph.nodes) if (node.id == entry.first) exists = true;
      if (exists) grade.positions.emplace(entry.first, position);
      else warnings.push_back("Dropped position for nonexistent node: " + entry.first);
    }
  }
  return grade;
}
}  // namespace

namespace persist_detail {
const char *operationName(NodeOperation operation, bool testOperations) {
  switch (operation) {
    case NodeOperation::Source: return "source";
    case NodeOperation::ImageProcessor: return "image-processor";
    default: break;
  }
  if (testOperations) {
    switch (operation) {
      case NodeOperation::TestImageToMask: return "test-image-to-mask";
      case NodeOperation::TestMaskProcessor: return "test-mask-processor";
      case NodeOperation::TestMultipleOutputs: return "test-multiple-outputs";
      case NodeOperation::TestSlotted: return "test-slotted";
      case NodeOperation::TestMaskedImage: return "test-masked-image";
      case NodeOperation::TestReferenceMask: return "test-reference-mask";
      case NodeOperation::TestImage: return "test-image";
      case NodeOperation::TestUnavailableImage: return "test-unavailable-image";
      case NodeOperation::TestUnavailableMask: return "test-unavailable-mask";
      default: break;
    }
  }
  throw std::runtime_error("Unsupported graph operation");
}
NodeOperation operationFromName(const std::string &name, bool testOperations) {
  // Existing enum vocabulary only, not a processor/operation registry.
  for (auto op : {NodeOperation::Source, NodeOperation::ImageProcessor,
       NodeOperation::TestImageToMask, NodeOperation::TestMaskProcessor, NodeOperation::TestMultipleOutputs,
       NodeOperation::TestSlotted, NodeOperation::TestMaskedImage, NodeOperation::TestReferenceMask,
       NodeOperation::TestImage, NodeOperation::TestUnavailableImage, NodeOperation::TestUnavailableMask}) {
    if (!testOperations && op != NodeOperation::Source && op != NodeOperation::ImageProcessor) continue;
    if (name == operationName(op, testOperations)) return op;
  }
  throw std::runtime_error("Unsupported graph operation: " + name);
}
std::string graphRecordError(const PersistGraph &graph, bool testOperations) {
  // Temporary Nodes let the authoritative structural validator check every
  // connection/type/cycle without instantiating any processor backend.
  std::vector<Node> nodes;
  for (const auto &record : graph.nodes) {
    (void)operationName(record.operation, testOperations);
    Node node(record.operation);
    node.id = NodeId{record.id}; node.enabled = record.enabled;
    node.fixedInputs = record.inputs; node.slots = record.slots;
    const bool slotted = !nodeInterface(record.operation).slotInputs.empty();
    if (slotted && !node.restoreNextSlotId(record.nextSlotId)) return "Invalid Slot allocator state";
    if (!slotted && (!record.slots.empty() || record.nextSlotId != 1)) return "Unexpected Slot state";
    if (record.operation == NodeOperation::ImageProcessor && (record.backend.empty() || record.identifier.empty()))
      return "Empty processor identity";
    nodes.push_back(std::move(node));
  }
  return graphStructureError({nodes, graph.primarySource, graph.documentOutput});
}
bool decodeGraphFile(const std::string &json, PersistFile &out, std::string &error,
                     std::vector<std::string> *warnings, bool testOperations) {
  Value root;
  if (!persist_json::read(json, root, error)) return false;
  try {
    fields(root, {"format", "version", "grade", "metadata"}, "file");
    int version = 0;
    if (textField(root, "format") != "rawnode" ||
        !persist_json::integer(field(root, "version"), version) || version != 1)
      throw std::runtime_error("Unsupported format/version");
    PersistFile file;
    std::vector<std::string> collected;
    const auto metadata = root.object.find("metadata");
    if (metadata != root.object.end()) {
      fields(metadata->second, {"mark"}, "metadata");
      if (metadata->second.object.count("mark")) {
        const auto mark = textField(metadata->second, "mark");
        if (mark != "pick" && mark != "reject" && mark != "unmarked") throw std::runtime_error("Unsupported metadata mark");
      }
      file.metadataJson = metadata->second.raw;
    }
    const auto grade = root.object.find("grade");
    if (grade != root.object.end()) file.grade = gradeRecord(grade->second, testOperations, collected);
    out = std::move(file);
    if (warnings) *warnings = std::move(collected);
    return true;
  } catch (const std::runtime_error &failure) {
    error = failure.what();
    return false;
  }
}

ParameterValue parameterValue(const ProcessorParameter &param, const std::string &raw) {
  Value value;
  std::string error;
  if (!persist_json::read(raw, value, error)) throw std::runtime_error(param.id + ": " + error);
  const auto invalid = [&]() -> void { throw std::runtime_error("Invalid parameter value: " + param.id); };
  switch (param.type) {
    case ParameterType::String:
    case ParameterType::Custom:
      if (value.type != Type::String) invalid();
      return value.string;
    case ParameterType::Boolean:
      if (value.type != Type::Boolean) invalid();
      return value.raw == "true";
    case ParameterType::Choice: {
      if (!param.choiceIds.empty()) {
        if (value.type != Type::String || param.choiceIds.size() != param.choices.size()) invalid();
        for (size_t i = 0; i < param.choiceIds.size(); ++i)
          if (value.string == param.choiceIds[i])
            return param.choiceValues.size() == param.choices.size() ? param.choiceValues[i] : int(i);
        throw std::runtime_error("Unknown installed choice ID: " + param.id + " = " + value.string);
      }
      int selected;
      if (!persist_json::integer(value, selected)) invalid();
      if (param.choiceValues.size() == param.choices.size() && !param.choiceValues.empty()) {
        if (std::find(param.choiceValues.begin(), param.choiceValues.end(), selected) == param.choiceValues.end()) invalid();
      } else if (selected < 0 || size_t(selected) >= param.choices.size()) invalid();
      return selected;
    }
    case ParameterType::Integer: {
      int number;
      if (!persist_json::integer(value, number)) invalid();
      return number;
    }
    case ParameterType::Double: {
      double number;
      if (!persist_json::finiteNumber(value, number)) invalid();
      return number;
    }
    case ParameterType::Vector: {
      const auto *current = std::get_if<std::vector<double>>(&param.value);
      if (value.type != Type::Array || !current || value.array.size() != current->size()) invalid();
      std::vector<double> numbers;
      for (const auto &element : value.array) {
        double number;
        if (!persist_json::finiteNumber(element, number) ||
            (param.vectorIsInteger && (std::trunc(number) != number || number < INT_MIN || number > INT_MAX))) invalid();
        numbers.push_back(number);
      }
      return numbers;
    }
    default: throw std::runtime_error("Non-persistent parameter: " + param.id);
  }
}
}  // namespace persist_detail

bool decodeGraphFile(const std::string &json, PersistFile &out, std::string &error,
                     std::vector<std::string> *warnings) {
  return persist_detail::decodeGraphFile(json, out, error, warnings, false);
}
