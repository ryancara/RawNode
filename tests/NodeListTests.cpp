#include "NodeListTests.h"

#include "DocumentMutation.h"
#include "GraphCommands.h"
#include "NodeGraph.h"
#include "RenderRuntimeTestAccess.h"
#include "persist/DocumentActions.h"
#include "processors/NativeExposureProcessor.h"
#include "ui/NodeList.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>

#define CHECK(expression) do { if (!(expression)) { \
  std::fprintf(stderr, "node list check failed at line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

namespace {

static OutputRef image(const NodeId &id) { return {id, ports::image}; }
static InputAddress input(const NodeId &id, const PortId &port = ports::image) { return {id, std::nullopt, port}; }
static GraphView view(const App &app) { return {app.nodes, app.primarySource, app.documentOutput}; }
static bool idle(App &app) {
  return RenderRuntimeTestAccess::wait(app.renderer, [](auto state) {
    return !state.previewPending && !state.previewBusy && !state.displayPending;
  });
}
static OutputRef publishedEndpoint(App &app) {
  std::lock_guard<std::mutex> lock(app.displayMutex);
  return app.processed.result.requested;
}
static Image raster(float value = 0.25f) {
  Image image; image.w = image.h = 1; image.px = {value, value, value, 1.0f};
  return image;
}
static void source(App &app) {
  app.full = raster();
  app.preview = std::make_shared<Image>(app.full);
}
static double exposure(const Node *node) {
  if (node && node->processor)
    for (const auto &parameter : node->processor->parameters())
      if (parameter.id == "exposure") return std::get<double>(parameter.value);
  return std::numeric_limits<double>::quiet_NaN();
}
static PersistNode persisted(const char *id, double gain = 0.0) {
  PersistNode node;
  node.id = id; node.backend = "native"; node.identifier = NativeExposureProcessor::kIdentifier;
  node.paramsJson["exposure"] = std::to_string(gain);
  return node;
}
static bool add(App &app, const NodeId &id, NodeOperation operation) {
  Node node(operation); node.id = id;
  return graph::addNode(app, std::move(node)).ok();
}

// Canonical observation of stored connections/controls for atomic-refusal and
// projection-purity checks. This does not compute a graph traversal or UI order.
static std::string topology(const App &app) {
  std::map<std::string, std::string> nodes;
  const auto bindings = [](const InputBindings &inputs) {
    std::string result;
    for (const auto &binding : inputs) {
      result += binding.first.value + "=";
      if (binding.second) result += binding.second->nodeId.value + "." + binding.second->outputPortId.value;
      result += ";";
    }
    return result;
  };
  for (const auto &node : app.nodes) {
    std::string state = std::to_string(static_cast<int>(node.operation)) + (node.enabled ? "+" : "-");
    state += bindings(node.fixedInputs);
    for (const auto &slot : node.slots)
      state += std::to_string(slot.id.value) + (slot.settings.enabled ? "+" : "-") + bindings(slot.inputs);
    nodes.emplace(node.id.value, std::move(state));
  }
  std::string result = app.primarySource.value + "/" + app.documentOutput.nodeId.value + "." + app.documentOutput.outputPortId.value;
  for (const auto &node : nodes) result += "\n" + node.first + ":" + node.second;
  return result;
}

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
      ("rawnode-6e1-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Files() { std::filesystem::create_directories(dir); }
  ~Files() { std::error_code error; std::filesystem::remove_all(dir, error); }
  std::string path(const char *name) const { return (dir / name).string(); }
};

static bool testSelectionIdentity() {
  App app; source(app);
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId;
  CHECK(addNativeExposureNode(app)); const NodeId b = *app.selectedNodeId;
  CHECK(findNode(app, a)->processor->setParameterValue("exposure", 1.5));
  CHECK(findNode(app, b)->processor->setParameterValue("exposure", -2.0));
  app.renderer.start(); CHECK(idle(app));
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(selectNode(app, a) && exposure(selectedNode(app)) == 1.5);
  app.paramFilter[0] = 'e';
  CHECK(selectNode(app, a) && app.paramFilter[0] == 'e');
  CHECK(!selectNode(app, app.primarySource) && !selectNode(app, NodeId{"missing"}));
  CHECK(app.selectedNodeId == a && app.paramFilter[0] == 'e');
  CHECK(selectNode(app, b) && app.paramFilter[0] == '\0' && exposure(selectedNode(app)) == -2.0);
  CHECK(selectNode(app, std::nullopt) && !selectedNode(app));
  CHECK(selectNode(app, a));
  const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(after.epoch == before.epoch && !after.previewPending && !after.previewBusy && !after.displayPending);

  app.paramFilter[0] = 'e';
  Processor *owned = selectedNode(app)->processor.get();
  {
    document_detail::DocumentMutation mutation(app);
    std::rotate(app.nodes.begin(), app.nodes.begin() + 2, app.nodes.end());
    mutation.changed();
  }
  CHECK(idle(app));
  CHECK(app.nodes.front().id == b && app.selectedNodeId == a && app.paramFilter[0] == 'e');
  CHECK(selectedNode(app)->processor.get() == owned && exposure(selectedNode(static_cast<const App &>(app))) == 1.5);
  CHECK(moveNode(app, a, b) && idle(app));
  CHECK(app.selectedNodeId == a && selectedNode(app)->processor.get() == owned && app.paramFilter[0] == 'e');
  CHECK(ui::projectNodeList(view(app)).spine == std::vector<NodeId>({b, a}));
  CHECK(add(app, NodeId{"unrelated"}, NodeOperation::ImageProcessor) && idle(app));
  CHECK(app.selectedNodeId == a && selectedNode(app)->processor.get() == owned);
  CHECK(selectedNode(app)->processor->setParameterValue("exposure", 2.25));
  CHECK(exposure(findNode(app, a)) == 2.25 && exposure(findNode(app, b)) == -2.0);
  return true;
}

static bool testDeletionAndInspection() {
  App app; source(app);
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId;
  CHECK(addNativeExposureNode(app)); const NodeId b = *app.selectedNodeId;
  CHECK(addNativeExposureNode(app)); const NodeId c = *app.selectedNodeId;
  CHECK(selectNode(app, b)); app.paramFilter[0] = 'x';
  app.renderer.requestInspection(image(a)); app.renderer.start(); CHECK(idle(app));
  CHECK(publishedEndpoint(app) == image(a));
  CHECK(destroyNode(app, a) && idle(app));
  CHECK(app.selectedNodeId == b && app.paramFilter[0] == 'x' && selectedNode(app)->id == b);
  CHECK(publishedEndpoint(app) == image(c) && findNode(app, b)->fixedInputs.at(ports::image) == image(app.primarySource));
  app.renderer.requestInspection(image(b)); CHECK(idle(app));
  CHECK(destroyNode(app, b) && idle(app));
  CHECK(!app.selectedNodeId && !selectedNode(app) && app.paramFilter[0] == '\0');
  CHECK(publishedEndpoint(app) == image(c));
  const NodeId other{"selected-off-spine"};
  CHECK(add(app, other, NodeOperation::TestReferenceMask) && selectNode(app, other));
  app.paramFilter[0] = 'x';
  app.renderer.requestInspection(image(app.primarySource)); CHECK(idle(app));
  CHECK(graph::deleteNode(app, other).ok() && idle(app));
  CHECK(!app.selectedNodeId && app.paramFilter[0] == '\0');
  CHECK(publishedEndpoint(app) == image(app.primarySource)); // A valid unrelated inspection survives.
  CHECK(destroyNode(app, c) && idle(app) && publishedEndpoint(app) == image(app.primarySource));
  return true;
}

static bool testProjectionAndOtherNodes() {
  App app;
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId;
  CHECK(addNativeCstNode(app)); const NodeId b = *app.selectedNodeId;
  CHECK(addNativeExposureNode(app)); const NodeId c = *app.selectedNodeId;
  const NodeId spare{"spare-image"}, mask{"auxiliary-mask"};
  Node extra; extra.id = spare; extra.processor = std::make_unique<NativeExposureProcessor>();
  CHECK(graph::addNode(app, std::move(extra)).ok() && add(app, mask, NodeOperation::TestReferenceMask));
  const auto before = topology(app);
  std::reverse(app.nodes.begin(), app.nodes.end()); // No worker exists in this fixture.
  const auto rows = ui::projectNodeList(view(app));
  CHECK(rows.spineError.empty() && rows.spine == std::vector<NodeId>({a, b, c}));
  CHECK(rows.other == std::vector<NodeId>({mask, spare}) && topology(app) == before);
  CHECK(selectNode(app, spare) && selectedNode(app)->processor);
  setNodeEnabled(app, spare, false);
  CHECK(!findNode(app, spare)->enabled && findNode(app, a)->enabled);
  const auto disabled = topology(app);
  CHECK(!moveNode(app, spare, a) && topology(app) == disabled);
  CHECK(app.getStatus().find("main Image spine") != std::string::npos);
  CHECK(destroyNode(app, spare) && !app.selectedNodeId);
  CHECK(selectNode(app, mask) && selectedNode(app)->id == mask && !selectedNode(app)->processor);
  CHECK(destroyNode(app, mask) && !app.selectedNodeId);
  CHECK(ui::projectNodeList(view(app)).other.empty() && ui::projectNodeList(view(app)).spine == rows.spine);
  CHECK(graph::setDocumentOutput(app, image(app.primarySource)).ok());
  const auto sourceOnly = ui::projectNodeList(view(app));
  CHECK(sourceOnly.spine.empty() && sourceOnly.other.size() == 3 && selectNode(app, b));
  CHECK(!destroyNode(app, app.primarySource) && app.selectedNodeId == b);
  return true;
}

static bool testIncompleteAndNonUnaryProjection() {
  App app;
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId;
  CHECK(addNativeCstNode(app)); const NodeId b = *app.selectedNodeId;
  CHECK(graph::disconnect(app, input(a)).ok());
  std::reverse(app.nodes.begin(), app.nodes.end());
  auto rows = ui::projectNodeList(view(app));
  CHECK(!rows.spineError.empty() && rows.spine == std::vector<NodeId>({a, b}));
  CHECK(selectNode(app, a) && selectedNode(app)->processor);
  auto before = topology(app); const auto epoch = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
  CHECK(!moveNode(app, b, a) && topology(app) == before);
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == epoch);
  CHECK(destroyNode(app, a) && !app.selectedNodeId && !findNode(app, b)->fixedInputs.at(ports::image));
  CHECK(selectNode(app, b)); before = topology(app);
  CHECK(!destroyNode(app, b) && app.selectedNodeId == b && topology(app) == before);
  CHECK(app.getStatus().find("explicit replacement") != std::string::npos);

  App mixed;
  CHECK(addNativeExposureNode(mixed)); const NodeId upstream = *mixed.selectedNodeId;
  const NodeId mixer{"mixer"};
  CHECK(add(mixed, mixer, NodeOperation::TestSlotted));
  const auto slot = graph::addSlot(mixed, mixer).addedSlot; CHECK(slot);
  CHECK(graph::connect(mixed, image(upstream), {mixer, slot, ports::image}).ok());
  CHECK(graph::setDocumentOutput(mixed, image(mixer)).ok());
  CHECK(addNativeExposureNode(mixed)); const NodeId tail = *mixed.selectedNodeId;
  CHECK(addNativeCstNode(mixed)); const NodeId end = *mixed.selectedNodeId;
  std::reverse(mixed.nodes.begin(), mixed.nodes.end());
  rows = ui::projectNodeList(view(mixed));
  CHECK(!rows.spineError.empty() && rows.spine == std::vector<NodeId>({tail, end}));
  CHECK(rows.other.size() == 2 && std::find(rows.other.begin(), rows.other.end(), mixer) != rows.other.end());
  CHECK(selectNode(mixed, mixer) && selectedNode(mixed)->operation == NodeOperation::TestSlotted);
  before = topology(mixed);
  CHECK(!moveNode(mixed, end, tail) && topology(mixed) == before);
  CHECK(destroyNode(mixed, mixer) && !mixed.selectedNodeId && !findNode(mixed, tail)->fixedInputs.at(ports::image));
  CHECK(ui::projectNodeList(view(mixed)).spine == rows.spine);
  return true;
}

static bool testRejectedTopologyEdits() {
  App app;
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId;
  const NodeId b{"masked-consumer"}, mask{"reference-to-a"};
  CHECK(add(app, b, NodeOperation::TestMaskedImage) && add(app, mask, NodeOperation::TestReferenceMask));
  CHECK(graph::connect(app, image(a), input(b)).ok());
  CHECK(graph::connect(app, image(a), input(mask, ports::reference)).ok());
  CHECK(graph::connect(app, {mask, ports::mask}, input(b, ports::mask)).ok());
  CHECK(graph::setDocumentOutput(app, image(b)).ok() && selectNode(app, a));
  const auto before = topology(app);
  CHECK(ui::projectNodeList(view(app)).spineError.empty());
  CHECK(!moveNode(app, a, b) && topology(app) == before && app.selectedNodeId == a);
  CHECK(app.getStatus().find("cycle") != std::string::npos);
  CHECK(!graph::reconnect(app, image(b), input(a)).ok() && topology(app) == before);
  CHECK(!graph::setDocumentOutput(app, {mask, ports::mask}).ok() && topology(app) == before);
  CHECK(!moveNode(app, a, NodeId{"deleted-drag-target"}) && topology(app) == before);
  return true;
}

static bool testLegacySelectionAndTransfers() {
  Files files;
  const auto legacy = files.path("legacy.ofxrawhost.json");
  {
    std::ofstream file(legacy);
    file << "{\"format\":\"ofxrawhost-sidecar\",\"version\":1,\"kind\":\"input\","
            "\"chain\":{\"selectedNode\":1,\"nodes\":[{\"pluginIdentifier\":\"missing.first\",\"params\":{}},"
            "{\"pluginIdentifier\":\"missing.second\",\"params\":{}}]}}";
    CHECK(file.good());
  }
  PersistSidecar loaded; CHECK(loadSidecarFile(legacy, loaded));
  App app; applyChain(app, loaded.chain);
  CHECK(app.nodes.size() == 3 && app.selectedNodeId == app.nodes[2].id);
  CHECK(selectedNode(app)->storedIdentifier == "missing.second");
  const NodeId migrated = *app.selectedNodeId;
  std::reverse(app.nodes.begin(), app.nodes.end());
  CHECK(app.selectedNodeId == migrated && selectedNode(app)->storedIdentifier == "missing.second");

  PersistChain chain; chain.nodes = {persisted("first", 1.25), persisted("second", -2.0)};
  chain.selectedNodeId = "second";
  CHECK(saveInputSidecar(files.path("v2.png"), {}, chain));
  CHECK(loadSidecarFile(inputSidecarPath(files.path("v2.png")), loaded));
  applyChain(app, loaded.chain);
  CHECK(app.selectedNodeId == NodeId{"second"} && exposure(selectedNode(app)) == -2.0);
  CHECK(moveNode(app, NodeId{"second"}, NodeId{"first"}));
  std::reverse(app.nodes.begin(), app.nodes.end());
  const auto captured = captureChain(app);
  CHECK(captured.selectedNodeId == "second" && captured.nodes.front().id == "second");
  PersistNode node; CHECK(captureNode(app, *app.selectedNodeId, node) && node.id == "second");
  PersistChain transfer; transfer.nodes = {node}; transfer.selectedNodeId = node.id;
  std::string kind; PersistChain decoded;
  CHECK(parseTransferPayload(serializeTransferPayload("node", transfer), kind, decoded) && kind == "node");
  CHECK(appendPersistedNode(app, decoded.nodes.front(), app.selectedNodeId));
  const NodeId pasted = *app.selectedNodeId;
  CHECK(pasted != NodeId{"second"} && exposure(selectedNode(app)) == -2.0);
  CHECK(findNode(app, pasted)->fixedInputs.at(ports::image) == image(NodeId{"second"}));
  CHECK(savePresetFile(files.path("node.rawnodepreset"), "node", transfer));
  CHECK(loadPresetFile(files.path("node.rawnodepreset"), kind, decoded));
  CHECK(appendPersistedNode(app, decoded.nodes.front(), app.selectedNodeId));
  CHECK(*app.selectedNodeId != pasted && exposure(selectedNode(app)) == -2.0);
  chain.selectedNodeId = "missing"; chain.selectedNode = 1;
  applyChain(app, chain); CHECK(app.selectedNodeId == NodeId{"second"}); // Legacy ordinal fallback.
  chain.selectedNode = 99;
  applyChain(app, chain); CHECK(app.selectedNodeId == NodeId{"first"}); // Historical default selection.
  return true;
}

static bool testPhotoAndDocumentReplacement() {
  Files files;
  const auto photoA = files.path("a.png"), photoB = files.path("b.png");
  const ColorEncoding encoding{RgbGamut::Rec709, TransferFunction::Linear};
  CHECK(writeImage(raster(0.25f), photoA, encoding, 92) && writeImage(raster(0.5f), photoB, encoding, 92));
  App app; openPath(app, photoA, true);
  PersistChain chain; chain.nodes = {persisted("shared-id", 1.0)}; chain.selectedNodeId = "shared-id";
  applyChain(app, chain);
  app.paramFilter[0] = 'x';
  app.renderer.requestInspection(image(NodeId{"shared-id"})); app.renderer.start(); CHECK(idle(app));
  openPath(app, photoB, true); CHECK(idle(app));
  CHECK(!app.selectedNodeId && app.paramFilter[0] == '\0' && publishedEndpoint(app) == image(app.primarySource));
  openPath(app, photoA, true); CHECK(idle(app));
  CHECK(app.selectedNodeId == NodeId{"shared-id"} && exposure(selectedNode(app)) == 1.0);

  PersistChain replacement;
  replacement.nodes = {persisted("shared-id", 4.0), persisted("new-output")};
  replacement.selectedNodeId = "shared-id";
  CHECK(saveInputSidecar(photoB, {}, replacement));
  app.renderer.requestInspection(image(NodeId{"shared-id"})); CHECK(idle(app));
  openPath(app, photoB, true); CHECK(idle(app));
  CHECK(app.selectedNodeId == NodeId{"shared-id"} && exposure(selectedNode(app)) == 4.0 && app.paramFilter[0] == '\0');
  CHECK(publishedEndpoint(app) == image(NodeId{"new-output"})); // Old ID exists, but old inspection is gone.

  app.renderer.requestInspection(image(NodeId{"shared-id"})); CHECK(idle(app));
  replacement.nodes[0].paramsJson["exposure"] = "-3.0";
  replacement.selectedNodeId = "new-output";
  applyChain(app, replacement); CHECK(idle(app));
  CHECK(app.selectedNodeId == NodeId{"new-output"} && exposure(selectedNode(app)) == 0.0);
  CHECK(exposure(findNode(app, NodeId{"shared-id"})) == -3.0 && publishedEndpoint(app) == image(NodeId{"new-output"}));

  CHECK(selectNode(app, NodeId{"shared-id"}));
  app.renderer.requestInspection(image(NodeId{"shared-id"})); CHECK(idle(app));
  openPath(app, photoA, false); CHECK(idle(app));
  CHECK(!app.selectedNodeId && publishedEndpoint(app) == image(NodeId{"new-output"}));
  applyChain(app, {}); CHECK(idle(app));
  CHECK(!app.selectedNodeId && !selectedNode(app) && publishedEndpoint(app) == image(app.primarySource));
  return true;
}

}  // namespace

bool testNodeList() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"6E.1 NodeId selection, storage/topology reorder and logical parameter association", testSelectionIdentity},
      {"6E.1 selected/unselected deletion clears only stale selection and inspection", testDeletionAndInspection},
      {"6E.1 topology List View keeps off-spine nodes visible, selectable and removable", testProjectionAndOtherNodes},
      {"6E.1 incomplete/non-unary List View retains nodes without guessed rewiring", testIncompleteAndNonUnaryProjection},
      {"6E.1 unsupported/cyclic UI reorders and graph edits preserve topology", testRejectedTopologyEdits},
      {"6E.1 V1/V2 selection compatibility and NodeId-addressed copy/paste/presets", testLegacySelectionAndTransfers},
      {"6E.1 photo/document replacement resets old selection and inspection", testPhotoAndDocumentReplacement},
  };
  for (const auto &test : cases) {
    if (!test.test()) { std::fprintf(stderr, "selftest FAILED: %s\n", test.name); return false; }
    std::printf("ok  %s\n", test.name);
  }
  return true;
}
