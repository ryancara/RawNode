#include "GraphPersistenceTests.h"
#include "GraphPersistenceTestAccess.h"

#include "GraphCommands.h"
#include "GraphEvaluator.h"
#include "GraphValidation.h"
#include "NodeGraph.h"
#include "ofx/OfxHost.h"
#include "persist/Json.h"
#include "processors/NativeCstProcessor.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::fprintf(stderr, "graph persistence check failed at line %d: %s\n", __LINE__, #expression); \
    return false; \
  } \
} while (false)

namespace {
// Hand-authored schema and node fixtures. Expected IDs, edges and numbers
// below are established explicitly, never obtained from an encoder's output.
const std::string source = R"({"id":"camera-root","kind":"source"})";
const std::string colors = R"("color":{"rawWorking":{"colorSpace":"rec2020","gamma":"linear"},"output":{"colorSpace":"rec709","gamma":"srgb"}})";
std::string file(const std::string &nodes, const std::string &output = "camera-root",
                 const std::string &positions = "", const std::string &port = "image") {
  return "{\"format\":\"rawnode\",\"version\":1,\"grade\":{" + colors +
      ",\"graph\":{\"primarySource\":\"camera-root\",\"documentOutput\":{\"node\":\"" + output +
      "\",\"port\":\"" + port + "\"},\"nodes\":[" + nodes + "]}" + positions + "}}";
}
const std::string exposure = R"({"id":"node-2","kind":"image-processor","backend":"native","identifier":"rawnode.native.exposure","enabled":true,"params":{"exposure":1},"inputs":{"image":{"node":"camera-root","port":"image"}}})";
std::string changed(std::string text, const std::string &from, const std::string &to) {
  const auto at = text.find(from);
  if (at == std::string::npos) throw std::logic_error("Bad test substitution: " + from);
  text.replace(at, from.size(), to);
  return text;
}
Node *node(document_detail::RestoredGraph &graph, const std::string &id) {
  for (auto &node : graph.nodes) if (node.id.value == id) return &node;
  return nullptr;
}
GraphView view(const document_detail::RestoredGraph &graph) { return {graph.nodes, graph.primarySource, graph.output}; }
const PersistNode *record(const PersistGraph &graph, const std::string &id) {
  for (const auto &node : graph.nodes) if (node.id == id) return &node;
  return nullptr;
}
bool rejects(const std::string &json, bool tests = false) {
  PersistFile destination;
  destination.metadataJson = R"({"mark":"pick"})";
  std::string error;
  const bool accepted = tests ? GraphPersistenceTestAccess::decode(json, destination, error)
                             : decodeGraphFile(json, destination, error);
  if (accepted || error.empty() || destination.grade || destination.metadataJson != R"({"mark":"pick"})") {
    std::fprintf(stderr, "unexpected accepted fixture or partial publication: %.220s\n", json.c_str());
    return false;
  }
  return true;
}
bool restoreRejects(const PersistGraph &graph) {
  try { (void)document_detail::restoreGraph(graph); return false; }
  catch (const std::runtime_error &error) { return *error.what() != '\0'; }
}
bool parameterRejects(const ProcessorParameter &param, const std::string &json) {
  try { (void)persist_detail::parameterValue(param, json); return false; }
  catch (const std::runtime_error &) { return true; }
}
ImageValue pixel() {
  auto image = std::make_shared<Image>();
  image->w = image->h = 1;
  image->px = {0.25f, -0.5f, 2.0f, 0.375f};
  return image;
}
// Toy arithmetic uses the existing evaluator's test-only interfaces. This
// proves execution follows the saved edges, not shuffled array order.
class Arithmetic final : public graph_test::Executor {
 public:
  std::map<std::string, int> calls;
  graph_test::OperationResult execute(const Node &node, const EvaluationContext &,
      const graph_test::InputValues &inputs, const RenderCancellation &) override {
    ++calls[node.id.value];
    auto image = std::make_shared<Image>(*(inputs.image ? inputs.image : inputs.reference));
    if (node.operation == NodeOperation::TestSlotted) {
      for (size_t i = 0; i < image->px.size(); ++i) if (i % 4 != 3) {
        image->px[i] = 0;
        for (const auto &slot : inputs.slots) image->px[i] = image->px[i] * 10 + slot.image->px[i];
      }
    } else {
      for (size_t i = 0; i < image->px.size(); ++i) if (i % 4 != 3)
        image->px[i] = node.id.value == "gain" ? image->px[i] * 2 : image->px[i] + 0.125f;
    }
    graph_test::OperationResult result;
    result.outputs.emplace(ports::image, ImageValue(image));
    return result;
  }
};

bool sourceAndMetadata() {
  PersistFile parsed;
  std::string error, json;
  CHECK(decodeGraphFile(file(source), parsed, error));
  CHECK(parsed.grade && !parsed.metadataJson);
  CHECK(parsed.grade->graph.primarySource == NodeId{"camera-root"});
  CHECK(parsed.grade->graph.documentOutput == (OutputRef{NodeId{"camera-root"}, ports::image}));
  CHECK(parsed.grade->graph.nodes.size() == 1);
  CHECK(parsed.grade->color.rawColorSpace == "rec2020" && parsed.grade->color.rawGamma == "linear");
  CHECK(parsed.grade->color.outputColorSpace == "rec709" && parsed.grade->color.outputGamma == "srgb");
  auto restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(graphStructureError(view(restored)).empty());
  CHECK(node(restored, "camera-root")->enabled);
  CHECK(encodeGraphFile(parsed, json, error));
  CHECK(json.find("groupOpen") == std::string::npos && json.find("selected") == std::string::npos);
  for (const auto &metadata : {"", ",\"metadata\":{}", ",\"metadata\":{\"mark\":\"pick\"}",
                              ",\"metadata\":{\"mark\":\"reject\"}", ",\"metadata\":{\"mark\":\"unmarked\"}"}) {
    CHECK(decodeGraphFile(std::string("{\"format\":\"rawnode\",\"version\":1") + metadata + "}", parsed, error));
    CHECK(!parsed.grade);
    CHECK(encodeGraphFile(parsed, json, error));
    CHECK(decodeGraphFile(json, parsed, error));
    CHECK(!parsed.grade);
  }
  CHECK(decodeGraphFile(changed(file(source), "\"grade\":", "\"metadata\":{\"mark\":\"pick\"},\"grade\":"), parsed, error));
  CHECK(parsed.grade && parsed.metadataJson == R"({"mark":"pick"})");
  // Check every registry pair independently against its stable identifiers.
  for (int g = 0; g < rgbGamutCount(); ++g) for (int t = 0; t < transferFunctionCount(); ++t) {
    const std::string gamut = rgbGamutDefinition(g).id, gamma = transferFunctionDefinition(t).id;
    auto input = changed(file(source), "rec2020", gamut);
    input = changed(input, "linear", gamma);
    CHECK(decodeGraphFile(input, parsed, error));
    CHECK(parsed.grade->color.rawColorSpace == gamut && parsed.grade->color.rawGamma == gamma);
    input = changed(file(source), "rec709", gamut);
    input = changed(input, "srgb", gamma);
    CHECK(decodeGraphFile(input, parsed, error));
    CHECK(parsed.grade->color.outputColorSpace == gamut && parsed.grade->color.outputGamma == gamma);
  }
  return true;
}

bool topologyAndIdentities() {
  PersistFile parsed;
  std::string error, encoded;
  // Both gain and offset fixture records are independent of codec output.
  const std::string gain = R"({"id":"gain","kind":"test-image","enabled":true,"inputs":{"image":{"node":"camera-root","port":"image"}}})";
  const std::string offset = R"({"id":"offset","kind":"test-image","enabled":true,"inputs":{"image":{"node":"gain","port":"image"}}})";
  CHECK(GraphPersistenceTestAccess::decode(file(offset + "," + source + "," + gain, "offset"), parsed, error));
  auto restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(restored.nodes[0].id == NodeId{"offset"} && restored.nodes[2].id == NodeId{"gain"});
  CHECK(node(restored, "offset")->fixedInputs.at(ports::image) == (OutputRef{NodeId{"gain"}, ports::image}));
  Arithmetic arithmetic;
  const auto result = evaluateGraph(view(restored), restored.output, {EvaluationPurpose::Inspection, 1, 1, 1, 1}, pixel(), {}, &arithmetic);
  CHECK(result.state == EvaluationState::Available);
  const auto output = std::get<ImageValue>(result.value);
  // Source -> multiply by 2 -> add 1/8: order reversal would give red 3/4.
  CHECK(output->px == (std::vector<float>{0.625f, -0.875f, 4.125f, 0.375f}));
  CHECK(arithmetic.calls["gain"] == 1 && arithmetic.calls["offset"] == 1);
  CHECK(GraphPersistenceTestAccess::encode(parsed, encoded, error));
  CHECK(rejects(encoded));  // Test-only names must never enter production files.

  const auto tail = changed(changed(exposure, "node-2", "node-3"), "\"node\":\"camera-root\"", "\"node\":\"node-2\"");
  const auto branch = changed(exposure, "node-2", "branch");
  const auto disabled = changed(changed(exposure, "node-2", "disabled"), "true", "false");
  const auto detached = changed(changed(exposure, "node-2", "detached"), "{\"node\":\"camera-root\",\"port\":\"image\"}", "null");
  CHECK(decodeGraphFile(file(tail + "," + detached + "," + source + "," + exposure + "," + branch + "," + disabled, "node-3"), parsed, error));
  restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(restored.nodes.size() == 6 && restored.output.nodeId == NodeId{"node-3"});
  CHECK(!node(restored, "disabled")->enabled && !node(restored, "detached")->fixedInputs.at(ports::image));
  CHECK(requestedGraphError(view(restored), restored.output).empty());
  CHECK(!requestedGraphError(view(restored), {NodeId{"detached"}, ports::image}).empty());
  const auto nativeResult = evaluateGraph(view(restored), restored.output, {EvaluationPurpose::Inspection, 1, 1, 1, 1}, pixel());
  CHECK(nativeResult.state == EvaluationState::Available);
  CHECK(std::get<ImageValue>(nativeResult.value)->px == (std::vector<float>{1, -2, 8, 0.375f})); // 2^1 * 2^1
  auto captured = captureGraph(view(restored));
  CHECK(captured.nodes.size() == 6 && captured.nodes[0].id == "node-3");
  CHECK(record(captured, "branch")->inputs.at(ports::image)->nodeId == NodeId{"camera-root"});
  // Fresh insertion cannot rename either existing node, even with shuffled storage.
  App app;
  app.nodes = std::move(restored.nodes); app.primarySource = restored.primarySource; app.documentOutput = restored.output;
  CHECK(appendPersistedNode(app, parsed.grade->graph.nodes[3], NodeId{"node-2"}));
  CHECK(findNode(app, NodeId{"node-3"}) && findNode(app, NodeId{"node-2"}));
  const auto predecessor = *findNode(app, NodeId{"node-3"})->fixedInputs.at(ports::image);
  CHECK(predecessor.nodeId != NodeId{"node-2"} && predecessor.nodeId != NodeId{"node-3"});
  CHECK(findNode(app, predecessor.nodeId)->fixedInputs.at(ports::image)->nodeId == NodeId{"node-2"});
  // Explicit null required input stays encodable even when it is documentOutput.
  CHECK(decodeGraphFile(file(source + "," + detached, "detached"), parsed, error));
  CHECK(encodeGraphFile(parsed, encoded, error));
  return true;
}

bool slotsAndDefaults() {
  const std::string gain = R"({"id":"gain","kind":"test-image","enabled":true,"inputs":{"image":{"node":"camera-root","port":"image"}}})";
  const std::string offset = R"({"id":"offset","kind":"test-image","enabled":true,"inputs":{"image":{"node":"camera-root","port":"image"}}})";
  const std::string mixer = R"({"id":"mix","kind":"test-slotted","enabled":true,"inputs":{"reference":{"node":"camera-root","port":"image"}},"slots":[{"id":"3","settings":{},"inputs":{"image":{"node":"gain","port":"image"}}},{"id":"1","settings":{"enabled":false},"inputs":{"image":{"node":"offset","port":"image"},"mask":null}}],"nextSlotId":"5"})";
  const auto fixture = file(mixer + "," + source + "," + offset + "," + gain, "mix");
  PersistFile parsed;
  std::string error, encoded;
  CHECK(GraphPersistenceTestAccess::decode(fixture, parsed, error));
  auto restored = document_detail::restoreGraph(parsed.grade->graph);
  auto &mix = *node(restored, "mix");
  CHECK(mix.slots.size() == 2 && mix.slots[0].id == SlotId{3} && mix.slots[1].id == SlotId{1});
  CHECK(mix.slots[0].settings.enabled && !mix.slots[1].settings.enabled);
  CHECK(!mix.slots[0].inputs.at(ports::mask));
  CHECK(mix.nextSlotIdForPersistence() == 5);
  mix.slots[1].settings.enabled = true;
  Arithmetic arithmetic;
  auto result = evaluateGraph(view(restored), restored.output, {EvaluationPurpose::Inspection, 1, 1, 1, 1}, pixel(), {}, &arithmetic);
  CHECK(result.state == EvaluationState::Available);
  // Ordered fold of [gain=1/2, offset=3/8] gives 10*(1/2)+3/8=43/8.
  CHECK(std::get<ImageValue>(result.value)->px[0] == 5.375f);
  CHECK(arithmetic.calls["gain"] == 1 && arithmetic.calls["offset"] == 1);
  mix.slots[1].settings.enabled = false;
  parsed.grade->graph = captureGraph(view(restored));
  CHECK(GraphPersistenceTestAccess::encode(parsed, encoded, error));
  CHECK(encoded.find("\"mask\": null") != std::string::npos); // Writers materialise optional inputs.
  CHECK(GraphPersistenceTestAccess::decode(encoded, parsed, error));
  restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(node(restored, "mix")->makeSlot().id == SlotId{5});
  // Delete all surviving Slots, capture and reopen. Allocation still starts at 5.
  restored = document_detail::restoreGraph(parsed.grade->graph);
  node(restored, "mix")->slots.clear();
  parsed.grade->graph = captureGraph(view(restored));
  CHECK(GraphPersistenceTestAccess::encode(parsed, encoded, error));
  CHECK(GraphPersistenceTestAccess::decode(encoded, parsed, error));
  restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(node(restored, "mix")->makeSlot().id == SlotId{5});

  // The existing optional Reference port and optional Slot Mask default to null.
  CHECK(GraphPersistenceTestAccess::decode(changed(fixture, R"("reference":{"node":"camera-root","port":"image"})", ""), parsed, error));
  restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(!node(restored, "mix")->fixedInputs.at(ports::reference));
  CHECK(node(restored, "mix")->slots[0].settings.enabled);
  const std::string masked = R"({"id":"effect","kind":"test-masked-image","enabled":true,"inputs":{"image":{"node":"camera-root","port":"image"}}})";
  CHECK(GraphPersistenceTestAccess::decode(file(source + "," + masked, "effect"), parsed, error));
  CHECK(!record(parsed.grade->graph, "effect")->inputs.at(ports::mask));

  for (const std::string &ids : {std::string("3"), std::string("18446744073709551615")}) {
    auto exhausted = changed(changed(fixture, "\"nextSlotId\":\"5\"", "\"nextSlotId\":\"18446744073709551615\""), "\"id\":\"3\"", "\"id\":\"" + ids + "\"");
    CHECK(GraphPersistenceTestAccess::decode(exhausted, parsed, error));
    restored = document_detail::restoreGraph(parsed.grade->graph);
    auto &exhaustedNode = *node(restored, "mix");
    CHECK(exhaustedNode.nextSlotIdForPersistence() == UINT64_MAX);
    exhaustedNode.slots.clear();
    auto captured = captureGraph(view(restored));
    restored = document_detail::restoreGraph(captured);
    bool threw = false;
    try { (void)node(restored, "mix")->makeSlot(); } catch (const std::overflow_error &) { threw = true; }
    CHECK(threw && node(restored, "mix")->nextSlotIdForPersistence() == UINT64_MAX);
  }
  // Capture observes manually staged IDs, just as StagedEdit and makeSlot do.
  Node staged(NodeOperation::TestSlotted);
  staged.slots.push_back({SlotId{12}, {{ports::image, std::nullopt}, {ports::mask, std::nullopt}}, {}});
  CHECK(staged.nextSlotIdForPersistence() == 13);
  CHECK(!staged.restoreNextSlotId(12) && !staged.restoreNextSlotId(0));

  for (const auto &replacement : {"0", "00", "01", "-1", "+1", "1.0", " 1", "18446744073709551616", "abc"})
    CHECK(rejects(changed(fixture, "\"id\":\"3\"", std::string("\"id\":\"") + replacement + "\""), true));
  for (const auto &replacement : {"0", "1", "3", "-1", "18446744073709551616"})
    CHECK(rejects(changed(fixture, "\"nextSlotId\":\"5\"", std::string("\"nextSlotId\":\"") + replacement + "\""), true));
  CHECK(rejects(changed(fixture, "\"id\":\"3\"", "\"id\":\"1\""), true));
  CHECK(rejects(changed(fixture, ",\"nextSlotId\":\"5\"", ""), true));
  CHECK(rejects(changed(fixture, "\"nextSlotId\":\"5\"", "\"nextSlotId\":5"), true));
  CHECK(rejects(changed(fixture, "\"slots\":[", "\"slots\":null,\"unused\":["), true));
  CHECK(rejects(changed(fixture, "\"settings\":{}", "\"settings\":{\"enabled\":1}"), true));
  CHECK(rejects(changed(fixture, "\"settings\":{}", "\"settings\":{\"strength\":0.5}"), true));
  CHECK(rejects(changed(fixture, "\"settings\":{},", ""), true));
  CHECK(rejects(changed(fixture, "\"mask\":null", "\"mask\":{\"node\":\"gain\",\"port\":\"image\"}"), true));
  CHECK(rejects(changed(fixture, "\"image\":{\"node\":\"gain\",\"port\":\"image\"}", ""), true));
  CHECK(rejects(changed(fixture, "\"image\":{\"node\":\"gain\",\"port\":\"image\"}", "\"image\":{\"node\":\"mix\",\"port\":\"image\"}"), true));
  CHECK(rejects(changed(fixture, R"("slots":[{"id":"3","settings":{},"inputs":{"image":{"node":"gain","port":"image"}}},{"id":"1","settings":{"enabled":false},"inputs":{"image":{"node":"offset","port":"image"},"mask":null}}],)", ""), true));
  // Slot identities are local to each node, never globally unique.
  CHECK(GraphPersistenceTestAccess::decode(file(mixer + "," + changed(mixer, "mix", "mix-copy") + "," + source + "," + gain + "," + offset, "mix"), parsed, error));
  CHECK(record(parsed.grade->graph, "mix-copy")->slots[0].id == SlotId{3});
  return true;
}

bool typedTopology() {
  // Named secondary Image output, required Mask input, optional coverage and
  // Reference links all survive storage shuffling and processor-free validation.
  const std::string fork = R"({"id":"fork","kind":"test-multiple-outputs","enabled":true,"inputs":{"image":{"node":"camera-root","port":"image"}}})";
  const std::string coverage = R"({"id":"coverage","kind":"test-mask-processor","enabled":false,"inputs":{"mask":{"node":"fork","port":"mask"}}})";
  const std::string mix = R"({"id":"mix","kind":"test-slotted","enabled":true,"inputs":{"reference":{"node":"fork","port":"original"}},"slots":[{"id":"3","settings":{},"inputs":{"image":{"node":"fork","port":"image"},"mask":{"node":"coverage","port":"mask"}}},{"id":"1","settings":{},"inputs":{"image":{"node":"fork","port":"original"}}}],"nextSlotId":"5"})";
  const std::string offSpine = R"({"id":"off","kind":"test-reference-mask","enabled":true,"inputs":{"reference":{"node":"fork","port":"original"}}})";
  const auto fixture = file(mix + "," + coverage + "," + source + "," + fork + "," + offSpine, "mix");
  PersistFile parsed;
  std::string error, encoded;
  CHECK(GraphPersistenceTestAccess::decode(fixture, parsed, error));
  auto restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(node(restored, "mix")->fixedInputs.at(ports::reference) == (OutputRef{NodeId{"fork"}, PortId{"original"}}));
  CHECK(node(restored, "mix")->slots[0].inputs.at(ports::mask) == (OutputRef{NodeId{"coverage"}, ports::mask}));
  CHECK(node(restored, "mix")->slots[1].inputs.at(ports::image) == (OutputRef{NodeId{"fork"}, PortId{"original"}}));
  CHECK(node(restored, "off")->fixedInputs.at(ports::reference)->nodeId == NodeId{"fork"});
  CHECK(!node(restored, "coverage")->enabled && restored.nodes.back().id == NodeId{"off"});
  parsed.grade->graph = captureGraph(view(restored));
  CHECK(GraphPersistenceTestAccess::encode(parsed, encoded, error));
  CHECK(GraphPersistenceTestAccess::decode(encoded, parsed, error));
  CHECK(record(parsed.grade->graph, "mix")->slots[0].inputs.at(ports::mask)->nodeId == NodeId{"coverage"});
  CHECK(rejects(changed(fixture, R"("node":"coverage","port":"mask")", R"("node":"fork","port":"image")"), true));
  CHECK(rejects(file(source + "," + fork, "fork", "", "mask"), true));
  CHECK(rejects(changed(fixture, R"("node":"fork","port":"mask")", R"("node":"coverage","port":"mask")"), true)); // Disabled Mask cycle.
  for (const auto &kind : {"test-image-to-mask", "test-mask-processor", "test-multiple-outputs", "test-slotted",
       "test-masked-image", "test-reference-mask", "test-image", "test-unavailable-image", "test-unavailable-mask"})
    CHECK(rejects(file(source + "," + changed(exposure, "image-processor", kind))));
  return true;
}

bool parametersAndPositions() {
  const std::string opaque = R"({"nested":[9007199254740993,0.123456789012345678901234567890,1e9999,{"text":"caf\u00e9 \ud83e\udd8a"}],"flag":true,"nothing":null})";
  const std::string missing = R"({"id":"lost","kind":"image-processor","backend":"ctl","identifier":"/unavailable/rawnode-codec-script.ctl","label":"caf\u00e9 \ud83e\udd8a","enabled":false,"params":{"opaque":)" + opaque + R"(},"inputs":{"image":{"node":"camera-root","port":"image"}}})";
  const std::string precise = "0.1000000000000000000000000000000000001";
  const std::string installed = changed(exposure, "\"exposure\":1", "\"exposure\":" + precise + ",\"future\":" + opaque);
  const std::string layout = R"(,"positions":{"node-2":{"x":-12.5,"y":8.25},"camera-root":{"x":0,"y":1e2},"deleted":{"x":1,"y":2}})";
  PersistFile parsed;
  std::string error, encoded;
  std::vector<std::string> warnings;
  CHECK(decodeGraphFile(file(missing + "," + source + "," + installed, "node-2", layout), parsed, error, &warnings));
  CHECK(warnings.size() == 1 && warnings[0].find("deleted") != std::string::npos);
  CHECK(parsed.grade->positions.size() == 2 && parsed.grade->positions.at("camera-root").y == 100);
  CHECK(parsed.grade->positions.at("node-2").x == -12.5 && parsed.grade->positions.at("node-2").y == 8.25);
  CHECK(record(parsed.grade->graph, "lost")->label == u8"café 🦊");
  auto restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(node(restored, "lost")->isMissingProcessor() && !node(restored, "lost")->enabled);
  CHECK(node(restored, "lost")->storedIdentifier == "/unavailable/rawnode-codec-script.ctl");
  CHECK(node(restored, "lost")->preservedParamsJson.at("opaque") == opaque);
  CHECK(node(restored, "node-2")->processor);
  CHECK(std::get<double>(node(restored, "node-2")->processor->parameters()[0].value) == 0.1);
  node(restored, "node-2")->groupOpen["transient"] = false;
  parsed.grade->graph = captureGraph(view(restored));
  CHECK(record(parsed.grade->graph, "node-2")->paramsJson.at("future") == opaque);
  CHECK(record(parsed.grade->graph, "node-2")->paramsJson.at("exposure") == precise);
  CHECK(record(parsed.grade->graph, "node-2")->groupOpen.empty());
  CHECK(encodeGraphFile(parsed, encoded, error));
  CHECK(encoded.find("groupOpen") == std::string::npos && encoded.find("9007199254740993") != std::string::npos);
  CHECK(decodeGraphFile(encoded, parsed, error));
  CHECK(record(parsed.grade->graph, "lost")->paramsJson.at("opaque") == opaque);
  CHECK(record(parsed.grade->graph, "node-2")->paramsJson.at("future") == opaque);
  CHECK(parsed.grade->positions.at("node-2").x == -12.5);
  CHECK(node(restored, "node-2")->processor->setParameterValue("exposure", 1.25));
  const auto captured = captureGraph(view(restored));
  CHECK(record(captured, "node-2")->paramsJson.at("exposure") == "1.25");
  CHECK(record(captured, "node-2")->paramsJson.at("future") == opaque);

  CHECK(decodeGraphFile(file(source + "," + changed(exposure, "native", "future-backend"), "node-2"), parsed, error));
  auto unavailable = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(node(unavailable, "node-2")->isMissingProcessor());
  CHECK(node(unavailable, "node-2")->storedBackend == "future-backend");
  const auto capturedUnavailable = captureGraph(view(unavailable));
  CHECK(record(capturedUnavailable, "node-2")->backend == "future-backend");
  CHECK(record(capturedUnavailable, "node-2")->paramsJson.at("exposure") == "1");

  const std::string cst = R"({"id":"cst","kind":"image-processor","backend":"native","identifier":"rawnode.native.cst","enabled":true,"params":{"input_space":"aces-ap1","input_gamma":"linear","output_space":"rec709","output_gamma":"srgb"},"inputs":{"image":{"node":"camera-root","port":"image"}}})";
  CHECK(decodeGraphFile(file(source + "," + cst, "cst"), parsed, error));
  restored = document_detail::restoreGraph(parsed.grade->graph);
  const auto params = node(restored, "cst")->processor->parameters();
  CHECK(std::get<int>(params[0].value) == int(RgbGamut::ACES_AP1));
  CHECK(std::get<int>(params[3].value) == int(TransferFunction::SRGB));
  const auto capturedCst = captureGraph(view(restored));
  CHECK(record(capturedCst, "cst")->paramsJson.at("input_space") == "\"aces-ap1\"");
  CHECK(decodeGraphFile(changed(file(source + "," + cst, "cst"), "aces-ap1", "future-gamut"), parsed, error));
  CHECK(restoreRejects(parsed.grade->graph)); // No installed default may replace an unknown choice.
  CHECK(encodeGraphFile(parsed, encoded, error)); // Processor-independent re-encoding preserves it.
  CHECK(encoded.find("future-gamut") != std::string::npos);
  for (const auto &bad : {"true", "\"1\"", "[]", "null", "{}", "1e9999", "1e-9999", "11"}) {
    CHECK(decodeGraphFile(file(source + "," + changed(exposure, "\"exposure\":1", std::string("\"exposure\":") + bad), "node-2"), parsed, error));
    CHECK(restoreRejects(parsed.grade->graph));
  }

  for (const auto &position : {"null", "[]", "1", "{}", R"({"x":0})", R"({"y":0})", R"({"x":"0","y":1})",
      R"({"x":null,"y":1})", R"({"x":[],"y":1})", R"({"x":1e9999,"y":1})", R"({"x":0,"y":0,"z":1})"})
    CHECK(rejects(file(source, "camera-root", std::string(",\"positions\":{\"camera-root\":") + position + '}')));
  for (const auto &positions : {"[]", "null", "0"})
    CHECK(rejects(file(source, "camera-root", std::string(",\"positions\":") + positions)));
  CHECK(decodeGraphFile(file(source), parsed, error));
  parsed.grade->positions["camera-root"] = {std::numeric_limits<double>::infinity(), 0};
  encoded = "unchanged";
  CHECK(!encodeGraphFile(parsed, encoded, error) && encoded == "unchanged" && !error.empty());
  parsed.grade->positions["camera-root"] = {0, std::numeric_limits<double>::quiet_NaN()};
  CHECK(!encodeGraphFile(parsed, encoded, error));
  parsed.grade->positions.clear();
  parsed.grade->positions["deleted"] = {0, 1};
  CHECK(encodeGraphFile(parsed, encoded, error, &warnings));
  CHECK(warnings.size() == 1 && encoded.find("deleted") == std::string::npos);
  CHECK(decodeGraphFile(changed(file(source), "camera-root", "caf\\u00e9"), parsed, error) == false); // Changed only primarySource.
  const auto unicodeSource = changed(changed(changed(file(source), "camera-root", "caf\\u00e9"), "camera-root", "café"), "camera-root", "café");
  CHECK(decodeGraphFile(unicodeSource, parsed, error));
  CHECK(parsed.grade->graph.primarySource == NodeId{u8"café"});
  return true;
}

bool strictRejections() {
  const auto valid = file(source + "," + exposure, "node-2");
  // Each prefix is truncated within a required outer object; none may publish.
  for (size_t length = 0; length < valid.size(); ++length) CHECK(rejects(valid.substr(0, length)));
  for (const auto &tail : {"x", "{}", "null", ",", "\v", "\f"}) CHECK(rejects(valid + tail));
  PersistFile parsed;
  std::string error;
  CHECK(decodeGraphFile(valid + " \n\r\t", parsed, error));
  for (const auto &input : {"null", "[]", "0", "{}", R"({"format":"rawnode"})",
      R"({"format":"rawnode","version":1,})", R"({"format":"rawnode" "version":1})",
      R"({"format":"rawnode","version":1,"format":"rawnode"})",
      R"({"format":"rawnode","version":1,"\u0066ormat":"rawnode"})",
      R"({"format":"rawnode","version":1,"grade":null})",
      R"({"format":"rawnode","version":1,"metadata":null})",
      R"({"format":"rawnode","version":1,"metadata":{"mark":"maybe"}})",
      R"({"format":"rawnode","version":1,"metadata":{"mark":0}})",
      R"({"format":"rawnode","version":1,"metadata":{"future":1}})"}) CHECK(rejects(input));
  for (const auto &version : {"0", "2", "-1", "1.0", "1e0", "true", "\"1\"", "null", "[]", "2147483648"})
    CHECK(rejects(changed(valid, "\"version\":1", std::string("\"version\":") + version)));
  CHECK(rejects(changed(valid, "rawnode\"", "rawnode-sidecar\"")));
  CHECK(rejects(changed(valid, "\"format\":\"rawnode\"", "\"format\":1")));
  for (const auto &number : {"+1", "01", "-01", ".1", "1.", "1e", "1e+", "NaN", "Infinity", "0x10", "--1", "1 2"})
    CHECK(rejects(changed(valid, "\"exposure\":1", std::string("\"exposure\":") + number)));
  for (const auto &text : {R"("\x20")", R"("\uD800")", R"("\uDC00")", R"("\uD800\u0061")", R"("\uZZZZ")", R"("\u123")"})
    CHECK(rejects(changed(valid, "\"exposure\":1", std::string("\"exposure\":") + text)));
  for (const auto &bytes : {std::string("\xc0\xaf"), std::string("\xe0\x80\x80"), std::string("\xed\xa0\x80"),
       std::string("\xf4\x90\x80\x80"), std::string("\x80"), std::string("\xe2\x82"), std::string("\xff")})
    CHECK(rejects(changed(valid, "\"exposure\":1", "\"exposure\":\"" + bytes + "\"")));
  for (int control = 0; control < 32; ++control)
    CHECK(rejects(changed(valid, "\"exposure\":1", "\"exposure\":\"a" + std::string(1, char(control)) + "b\"")));
  CHECK(rejects(changed(valid, "\"exposure\":1", R"("exposure":{"a":1,"\u0061":2})")));
  CHECK(rejects(changed(valid, "\"exposure\":1", std::string("\"exposure\":") + std::string(140, '[') + "0" + std::string(140, ']'))));
  CHECK(rejects(changed(valid, "\"exposure\":1", R"("exposure":{"é":1,"\u00e9":2})")));
  for (const auto &replacement : std::vector<std::pair<std::string, std::string>>{
      {R"("primarySource":"camera-root",)", ""},
      {R"("documentOutput":{"node":"node-2","port":"image"},)", ""},
      {R"("nodes":[)", R"("removed":[)"},
      {R"("graph":{)", R"("removed":{)"},
      {R"("color":{)", R"("removed":{)"},
      {R"("colorSpace":"rec2020",)", ""},
      {R"(,"gamma":"linear")", ""},
      {R"("primarySource":"camera-root")", R"("primarySource":null)"},
      {R"("documentOutput":{"node":"node-2","port":"image"})", R"("documentOutput":null)"},
      {R"("documentOutput":{"node":"node-2","port":"image"})", R"("documentOutput":{"node":"node-2"})"},
      {R"("documentOutput":{"node":"node-2","port":"image"})", R"("documentOutput":{"node":2,"port":"image"})"},
      {R"("documentOutput":{"node":"node-2","port":"image"})", R"("documentOutput":{"node":"node-2","port":null})"},
      {R"("colorSpace":"rec2020")", R"("colorSpace":2)"},
      {R"("backend":"native")", R"("backend":false)"},
      {R"("kind":"image-processor")", R"("kind":0)"},
      {R"("identifier":"rawnode.native.exposure")", R"("identifier":null)"},
      {R"("enabled":true)", R"("label":1,"enabled":true)"},
      {R"("enabled":true)", R"("ui":{},"enabled":true)"},
      {R"("enabled":true)", R"("slots":[],"nextSlotId":"1","enabled":true)"}})
    CHECK(rejects(changed(valid, replacement.first, replacement.second)));
  // Structural fields are closed at every level, including topology/UI data.
  for (const auto &key : {"grade", "graph", "color", "rawWorking", "output", "documentOutput", "params", "inputs"}) {
    // params is the explicit opaque exception; use duplicate keys there instead.
    const auto from = std::string("\"") + key + "\":{";
    const auto extra = std::string(key) == "params" ? "\"exposure\":0," : "\"unknown\":0,";
    CHECK(rejects(changed(valid, from, from + extra)));
  }
  CHECK(rejects(changed(valid, "\"version\":1", "\"version\":1,\"future\":1")));
  for (const auto &key : {"id", "kind", "backend", "identifier", "enabled", "params", "inputs"}) {
    persist_json::Value root;
    CHECK(persist_json::read(exposure, root, error));
    const auto &value = root.object.at(key);
    const auto member = std::string("\"") + key + "\":" + value.raw;
    const auto incomplete = exposure.find(member + ',') != std::string::npos
        ? changed(exposure, member + ',', "") : changed(exposure, ',' + member, "");
    CHECK(rejects(file(source + "," + incomplete, "node-2")));
  }
  for (const auto &badSource : {"", "\"id\":\"camera-root\",\"enabled\":true", "\"id\":\"camera-root\",\"kind\":\"source\",\"params\":{}"})
    CHECK(rejects(file("{" + std::string(badSource) + "}")));
  CHECK(rejects(file(exposure, "node-2")));
  CHECK(rejects(file(source + "," + source)));
  CHECK(rejects(file(changed(source, "camera-root", ""))));
  CHECK(rejects(file(source + "," + changed(exposure, "node-2", "camera-root"))));
  CHECK(rejects(file(source + "," + exposure, "missing")));
  CHECK(rejects(file(source, "camera-root", "", "mask")));
  CHECK(rejects(file(source + "," + exposure, "node-2", "", "image-in")));
  CHECK(rejects(file(source + "," + changed(exposure, "\"node\":\"camera-root\"", "\"node\":\"absent\""), "node-2")));
  CHECK(rejects(file(source + "," + changed(exposure, "\"port\":\"image\"", "\"port\":\"mask\""), "node-2")));
  CHECK(rejects(file(source + "," + changed(exposure, "\"inputs\":{\"image\"", "\"inputs\":{\"unknown\""), "node-2")));
  CHECK(rejects(file(source + "," + changed(exposure, "\"inputs\":{\"image\":{\"node\":\"camera-root\",\"port\":\"image\"}}", "\"inputs\":{}"), "node-2")));
  CHECK(rejects(file(source + "," + changed(exposure, "true", "1"), "node-2")));
  CHECK(rejects(file(source + "," + changed(exposure, "\"native\"", "\"\""), "node-2")));
  CHECK(rejects(file(source + "," + changed(exposure, "rawnode.native.exposure", ""), "node-2")));
  CHECK(rejects(file(source + "," + changed(exposure, "\"params\":{\"exposure\":1}", "\"params\":[]"), "node-2")));
  CHECK(rejects(changed(valid, "rec2020", "unknown-colour")));
  CHECK(rejects(changed(valid, "srgb", "unknown-gamma")));
  CHECK(rejects(changed(valid, "\"rawWorking\"", "\"removed\"")));
  CHECK(rejects(changed(valid, "\"output\"", "\"removed\"")));
  CHECK(rejects(file("")));
  const auto cycleA = changed(exposure, "\"node\":\"camera-root\"", "\"node\":\"node-2\"");
  CHECK(rejects(file(source + "," + cycleA, "node-2")));
  CHECK(rejects(file(source + "," + changed(cycleA, "true", "false")))); // Disabled, off-spine cycle.
  const auto cycleB = changed(changed(exposure, "node-2", "b"), "\"node\":\"camera-root\"", "\"node\":\"node-2\"");
  CHECK(rejects(file(source + "," + changed(exposure, "\"node\":\"camera-root\"", "\"node\":\"b\"") + "," + cycleB)));
  CHECK(rejects(changed(valid, "image-processor", "unknown-operation")));
  CHECK(rejects(file(source + R"(,{"id":"mask","kind":"test-reference-mask","enabled":true,"inputs":{}})")));
  // Save checks structure, colour, UTF-8 and raw fragments before publishing
  // any output string, rather than producing a file the reader would reject.
  CHECK(decodeGraphFile(valid, parsed, error));
  const auto original = parsed;
  std::string encoded = "untouched";
  parsed.grade->graph.nodes.push_back(parsed.grade->graph.nodes.back());
  CHECK(!encodeGraphFile(parsed, encoded, error) && encoded == "untouched");
  parsed = original;
  parsed.grade->color.outputGamma = "unknown";
  CHECK(!encodeGraphFile(parsed, encoded, error) && encoded == "untouched");
  parsed = original;
  parsed.grade->graph.nodes.back().paramsJson["bad"] = "1,\"injected\":2";
  CHECK(!encodeGraphFile(parsed, encoded, error) && encoded == "untouched");
  parsed = original;
  parsed.grade->graph.nodes.back().label = std::string("\xff");
  CHECK(!encodeGraphFile(parsed, encoded, error) && encoded == "untouched");
  return true;
}

bool typedParameters() {
  ProcessorParameter param;
  param.id = "checked";
  for (const auto type : {ParameterType::String, ParameterType::Custom}) {
    param.type = type;
    CHECK(std::get<std::string>(persist_detail::parameterValue(param, R"("\ud83e\udd8a")")) == u8"🦊");
    CHECK(parameterRejects(param, "1"));
  }
  param.type = ParameterType::Boolean;
  CHECK(std::get<bool>(persist_detail::parameterValue(param, "true")));
  CHECK(parameterRejects(param, "1"));
  param.type = ParameterType::Integer;
  CHECK(std::get<int>(persist_detail::parameterValue(param, "-2147483648")) == std::numeric_limits<int>::min());
  CHECK(parameterRejects(param, "2147483648") && parameterRejects(param, "1.2"));
  param.type = ParameterType::Vector; param.value = std::vector<double>{0, 0};
  CHECK(std::get<std::vector<double>>(persist_detail::parameterValue(param, "[-0.5,2]")) == (std::vector<double>{-0.5, 2}));
  CHECK(parameterRejects(param, "[1]") && parameterRejects(param, "[1,2,3]") && parameterRejects(param, "[1,true]"));
  param.vectorIsInteger = true;
  CHECK(parameterRejects(param, "[1,0.5]") && parameterRejects(param, "[2147483648,1]"));
  param.type = ParameterType::Choice; param.choices = {"first", "second"}; param.choiceValues = {2, 4};
  CHECK(std::get<int>(persist_detail::parameterValue(param, "4")) == 4);
  CHECK(parameterRejects(param, "3"));
  param.choiceIds = {"first-id", "second-id"};
  CHECK(std::get<int>(persist_detail::parameterValue(param, "\"second-id\"")) == 4);
  CHECK(parameterRejects(param, "4") && parameterRejects(param, "\"unknown\""));
  return true;
}
}  // namespace

bool testGraphPersistence() {
  CHECK(sourceAndMetadata());
  CHECK(topologyAndIdentities());
  CHECK(slotsAndDefaults());
  CHECK(typedTopology());
  CHECK(parametersAndPositions());
  CHECK(strictRejections());
  CHECK(typedParameters());
  std::puts("ok  Step 7 graph persistence: independent topology/pixel fixtures, Slots/defaults, strict JSON, opaque parameters and positions");
  return true;
}

bool testGraphPersistenceOfx() {
  CHECK(!gPlugins.empty());
  PersistFile parsed;
  std::string error;
  // A matching installed display label must not bind an unrelated identifier.
  auto missing = changed(changed(exposure, "\"native\"", "\"ofx\""), "rawnode.native.exposure", "org.rawnode.codec.missing");
  missing = changed(missing, "\"params\":", "\"label\":" + jsonStringValue(gPlugins[0].label) + ",\"params\":");
  CHECK(decodeGraphFile(file(source + "," + missing, "node-2"), parsed, error));
  auto restored = document_detail::restoreGraph(parsed.grade->graph);
  CHECK(node(restored, "node-2")->isMissingProcessor());
  CHECK(node(restored, "node-2")->storedIdentifier == "org.rawnode.codec.missing");
  std::puts("ok  Step 7 OFX restoration resolves identifier, never matching display label");
  return true;
}
