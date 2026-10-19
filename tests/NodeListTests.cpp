#include "NodeListTests.h"

#include "DocumentMutation.h"
#include "GraphCommands.h"
#include "GraphDepthFixtures.h"
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

// Expected bindings are specified directly from the approved serial-insertion
// table. Deliberately reverse storage so list/vector order cannot be the oracle.
static bool testSelectionAwareInsertion() {
  for (const auto viewMode : {NodesView::List, NodesView::Graph}) {
    for (int selection = 0; selection < 6; ++selection) {
      App app; app.nodesView = viewMode;
      CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
      CHECK(addNativeCstNode(app)); const auto b = *app.selectedNodeId();
      CHECK(addNativeExposureNode(app)); const auto c = *app.selectedNodeId();
      CHECK(findNode(app, a)->processor->setParameterValue("exposure", 1.5));
      Processor *original = findNode(app, a)->processor.get();
      const NodeId branch{"fanout"}, slots{"slots"};
      CHECK(add(app, branch, NodeOperation::TestImage) && add(app, slots, NodeOperation::TestSlotted));
      CHECK(graph::connect(app, image(a), input(branch)).ok());
      const auto slot = graph::addSlot(app, slots, {false}).addedSlot; CHECK(slot);
      CHECK(graph::connect(app, image(a), {slots, slot, ports::image}).ok());
      CHECK(graph::addSlot(app, slots).ok());
      const auto nextSlot = findNode(app, slots)->nextSlotIdForPersistence();
      app.nodePositions = {{a.value, {12, -7}}, {b.value, {98, 15}}};
      std::reverse(app.nodes.begin(), app.nodes.end());
      if (selection == 0) CHECK(selectNode(app, std::nullopt));
      if (selection == 1) selectOutput(app);
      if (selection == 2) CHECK(selectNode(app, app.primarySource));
      if (selection == 3) CHECK(selectNode(app, a));
      if (selection == 4) CHECK(selectNode(app, b));
      if (selection == 5) CHECK(selectNode(app, c));
      const auto count = app.nodes.size();
      CHECK(addNativeExposureNode(app)); const auto inserted = *app.selectedNodeId();
      const auto predecessor = selection == 2 ? app.primarySource : selection == 3 ? a : selection == 4 ? b : c;
      CHECK(app.nodes.size() == count + 1 && findNode(app, inserted)->fixedInputs.at(ports::image) == image(predecessor));
      CHECK(app.documentOutput == image(selection >= 2 && selection <= 4 ? c : inserted));
      CHECK(findNode(app, a)->fixedInputs.at(ports::image) == image(selection == 2 ? inserted : app.primarySource));
      CHECK(findNode(app, b)->fixedInputs.at(ports::image) == image(selection == 3 ? inserted : a));
      CHECK(findNode(app, c)->fixedInputs.at(ports::image) == image(selection == 4 ? inserted : b));
      CHECK(findNode(app, branch)->fixedInputs.at(ports::image) == image(a));
      const auto *slotted = findNode(app, slots);
      CHECK(slotted->slots.size() == 2 && slotted->slots[0].id == *slot && !slotted->slots[0].settings.enabled);
      CHECK(slotted->slots[0].inputs.at(ports::image) == image(a) && !slotted->slots[1].inputs.at(ports::image));
      CHECK(slotted->nextSlotIdForPersistence() == nextSlot);
      CHECK(findNode(app, a)->processor.get() == original && exposure(findNode(app, a)) == 1.5);
      CHECK(app.nodePositions.size() == 2 && app.nodePositions.at(a.value).x == 12 && app.nodePositions.at(a.value).y == -7);
      CHECK(app.nodePositions.at(b.value).x == 98 && app.nodePositions.at(b.value).y == 15);
      CHECK(documentStateError(app).empty());
      // The successful selection itself is the insertion point for the next Add.
      CHECK(addNativeCstNode(app)); const auto repeated = *app.selectedNodeId();
      CHECK(findNode(app, repeated)->fixedInputs.at(ports::image) == image(inserted));
      if (selection == 2) CHECK(findNode(app, a)->fixedInputs.at(ports::image) == image(repeated));
      else if (selection == 3) CHECK(findNode(app, b)->fixedInputs.at(ports::image) == image(repeated));
      else if (selection == 4) CHECK(findNode(app, c)->fixedInputs.at(ports::image) == image(repeated));
      else CHECK(app.documentOutput == image(repeated));
    }
    for (int selection = 0; selection < 3; ++selection) {
      App app; app.nodesView = viewMode;
      if (selection == 1) CHECK(selectNode(app, app.primarySource));
      if (selection == 2) selectOutput(app);
      CHECK(addNativeCstNode(app));
      CHECK(app.documentOutput == image(*app.selectedNodeId()));
      CHECK(selectedNode(app)->fixedInputs.at(ports::image) == image(app.primarySource));
    }
  }
  return true;
}

static bool testOffChainInsertion() {
  App app;
  CHECK(addNativeCstNode(app)); const auto output = app.documentOutput;
  const NodeId branch{"insertion"}, consumer{"existing-consumer"};
  Node exposureNode; exposureNode.id = branch;
  exposureNode.processor = std::make_unique<NativeExposureProcessor>();
  exposureNode.fixedInputs.at(ports::image) = image(app.primarySource);
  CHECK(graph::addNode(app, std::move(exposureNode)).ok());
  CHECK(add(app, consumer, NodeOperation::TestImage));
  CHECK(graph::connect(app, image(branch), input(consumer)).ok());
  CHECK(selectNode(app, branch));
  CHECK(addNativeExposureNode(app)); const auto inserted = *app.selectedNodeId();
  CHECK(app.documentOutput == output && selectedNode(app)->fixedInputs.at(ports::image) == image(branch));
  CHECK(findNode(app, consumer)->fixedInputs.at(ports::image) == image(branch));
  CHECK(findNode(app, output.nodeId)->fixedInputs.at(ports::image) == image(app.primarySource));
  CHECK(addNativeCstNode(app)); const auto repeated = *app.selectedNodeId();
  CHECK(app.documentOutput == output && selectedNode(app)->fixedInputs.at(ports::image) == image(inserted));
  CHECK(findNode(app, consumer)->fixedInputs.at(ports::image) == image(branch));
  // Off-chain includes a disconnected Image processor; do not attach it to Source.
  CHECK(graph::disconnect(app, input(branch)).ok() && selectNode(app, branch));
  CHECK(addNativeExposureNode(app));
  CHECK(selectedNode(app)->fixedInputs.at(ports::image) == image(branch) && app.documentOutput == output);
  CHECK(!findNode(app, branch)->fixedInputs.at(ports::image));
  CHECK(findNode(app, repeated)->fixedInputs.at(ports::image) == image(inserted));
  CHECK(documentStateError(app).empty());
  return true;
}

static bool testInvalidInsertionSelection() {
  for (bool queued : {false, true}) {
    App app; source(app);
    CHECK(addNativeExposureNode(app));
    const NodeId maskOnly{"mask-only"};
    CHECK(add(app, maskOnly, NodeOperation::TestReferenceMask));
    if (!queued) { app.renderer.start(); CHECK(idle(app)); }
    const auto demand = RenderRuntimeTestAccess::snapshot(app.renderer);
    CHECK(demand.previewPending == queued);
    for (const auto &id : {maskOnly, NodeId{"stale-selection"}}) {
      app.selection = id; app.paramFilter[0] = 'x';
      const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
      const auto connections = topology(app);
      int mutations = 0;
      RenderRuntimeTestAccess::observe(app.renderer, &mutations, [](void *context, auto event) noexcept {
        if (event == RenderRuntimeTestAccess::Event::MutationCompleted) ++*static_cast<int *>(context);
      });
      CHECK(!addNativeExposureNode(app) && !addNativeCstNode(app) && !addCtlNode(app, "never-constructed.ctl"));
      const auto after = RenderRuntimeTestAccess::snapshot(app.renderer);
      RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
      CHECK(app.getStatus().find("Selected node has no compatible Image output") != std::string::npos);
      CHECK(topology(app) == connections && app.selectedNodeId() == id && app.paramFilter[0] == 'x');
      CHECK(mutations == 0 && before.epoch == after.epoch && after.mutationDepth == 0);
      CHECK(before.previewPending == after.previewPending && before.displayPending == after.displayPending &&
            before.quietPending == after.quietPending);
    }
  }
  return true;
}

static bool testInsertionDepthRefusal() {
  depth_test::Chain chain(kMaxGraphDependencyDepth);
  App app;
  app.nodes = std::move(chain.nodes); app.primarySource = chain.source; app.documentOutput = chain.output;
  const auto before = topology(app);
  int mutations = 0;
  RenderRuntimeTestAccess::observe(app.renderer, &mutations, [](void *context, auto event) noexcept {
    if (event == RenderRuntimeTestAccess::Event::MutationCompleted) ++*static_cast<int *>(context);
  });
  // Appending and splicing anywhere on this full-depth chain must fail before
  // constructing a backend, changing selection, or entering mutation ownership.
  for (int selection = 0; selection < 4; ++selection) {
    if (selection == 0) selectNode(app, std::nullopt);
    if (selection == 1) selectOutput(app);
    if (selection == 2) selectNode(app, app.primarySource);
    if (selection == 3) selectNode(app, depth_test::id(64));
    const auto selected = app.selection;
    CHECK(!addNativeExposureNode(app) && !addCtlNode(app, "not-constructed.ctl"));
    CHECK(depth_test::depthError(app.getStatus()) && app.selection == selected && topology(app) == before);
  }
  RenderRuntimeTestAccess::observe(app.renderer, nullptr, nullptr);
  const auto state = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(mutations == 0 && state.epoch == 0 && !state.previewPending && state.mutationDepth == 0);
  return true;
}

static bool testInsertionSaveReopen() {
  Files files; App app;
  const auto photo = files.path("insertion.png");
  CHECK(writeImage(raster(), photo, {RgbGamut::Rec709, TransferFunction::Linear}, 92));
  CHECK(openPath(app, photo));
  CHECK(addNativeExposureNode(app)); const auto a = *app.selectedNodeId();
  CHECK(addNativeCstNode(app)); const auto b = *app.selectedNodeId();
  CHECK(findNode(app, a)->processor->setParameterValue("exposure", 1.75));
  CHECK(selectNode(app, a) && addNativeExposureNode(app)); const auto inserted = *app.selectedNodeId();
  CHECK(findNode(app, inserted)->processor->setParameterValue("exposure", -0.75));
  const auto serial = topology(app);
  CHECK(saveCurrentInputSidecar(app) && openPath(app, photo) && topology(app) == serial);
  CHECK(findNode(app, b)->fixedInputs.at(ports::image) == image(inserted));
  // Source -> CST -> Output, with the former chain now off-chain.
  CHECK(graph::reconnect(app, image(app.primarySource), input(b)).ok());
  CHECK(selectNode(app, a) && addNativeCstNode(app)); const auto branch = *app.selectedNodeId();
  const auto before = topology(app);
  CHECK(saveCurrentInputSidecar(app) && openPath(app, photo));
  CHECK(topology(app) == before && app.documentOutput == image(b) && !app.selectedNodeId());
  CHECK(findNode(app, inserted)->fixedInputs.at(ports::image) == image(a));
  CHECK(findNode(app, branch)->fixedInputs.at(ports::image) == image(a));
  CHECK(exposure(findNode(app, a)) == 1.75 && exposure(findNode(app, inserted)) == -0.75);
  return true;
}

static bool testSelectionIdentity() {
  App app; source(app);
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app)); const NodeId b = *app.selectedNodeId();
  CHECK(findNode(app, a)->processor->setParameterValue("exposure", 1.5));
  CHECK(findNode(app, b)->processor->setParameterValue("exposure", -2.0));
  app.renderer.start(); CHECK(idle(app));
  const auto before = RenderRuntimeTestAccess::snapshot(app.renderer);
  CHECK(selectNode(app, a) && exposure(selectedNode(app)) == 1.5);
  app.paramFilter[0] = 'e';
  CHECK(selectNode(app, a) && app.paramFilter[0] == 'e');
  CHECK(!selectNode(app, NodeId{"missing"}));
  CHECK(app.selectedNodeId() == a && app.paramFilter[0] == 'e');
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
  CHECK(app.nodes.front().id == b && app.selectedNodeId() == a && app.paramFilter[0] == 'e');
  CHECK(selectedNode(app)->processor.get() == owned && exposure(selectedNode(static_cast<const App &>(app))) == 1.5);
  CHECK(moveNode(app, a, b) && idle(app));
  CHECK(app.selectedNodeId() == a && selectedNode(app)->processor.get() == owned && app.paramFilter[0] == 'e');
  CHECK(ui::projectNodeList(view(app)).spine == std::vector<NodeId>({b, a}));
  CHECK(add(app, NodeId{"unrelated"}, NodeOperation::ImageProcessor) && idle(app));
  CHECK(app.selectedNodeId() == a && selectedNode(app)->processor.get() == owned);
  CHECK(selectedNode(app)->processor->setParameterValue("exposure", 2.25));
  CHECK(exposure(findNode(app, a)) == 2.25 && exposure(findNode(app, b)) == -2.0);
  return true;
}

static bool testDeletionAndInspection() {
  App app; source(app);
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app)); const NodeId b = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app)); const NodeId c = *app.selectedNodeId();
  CHECK(selectNode(app, b)); app.paramFilter[0] = 'x';
  app.renderer.requestInspection(image(a)); app.renderer.start(); CHECK(idle(app));
  CHECK(publishedEndpoint(app) == image(a));
  CHECK(destroyNode(app, a) && idle(app));
  CHECK(app.selectedNodeId() == b && app.paramFilter[0] == 'x' && selectedNode(app)->id == b);
  CHECK(publishedEndpoint(app) == image(c) && findNode(app, b)->fixedInputs.at(ports::image) == image(app.primarySource));
  app.renderer.requestInspection(image(b)); CHECK(idle(app));
  CHECK(destroyNode(app, b) && idle(app));
  CHECK(!app.selectedNodeId() && !selectedNode(app) && app.paramFilter[0] == '\0');
  CHECK(publishedEndpoint(app) == image(c));
  const NodeId other{"selected-off-spine"};
  CHECK(add(app, other, NodeOperation::TestReferenceMask) && selectNode(app, other));
  app.paramFilter[0] = 'x';
  app.renderer.requestInspection(image(app.primarySource)); CHECK(idle(app));
  CHECK(graph::deleteNode(app, other).ok() && idle(app));
  CHECK(!app.selectedNodeId() && app.paramFilter[0] == '\0');
  CHECK(publishedEndpoint(app) == image(app.primarySource)); // A valid unrelated inspection survives.
  CHECK(destroyNode(app, c) && idle(app) && publishedEndpoint(app) == image(app.primarySource));
  return true;
}

static bool testProjectionAndOtherNodes() {
  App app;
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId();
  CHECK(addNativeCstNode(app)); const NodeId b = *app.selectedNodeId();
  CHECK(addNativeExposureNode(app)); const NodeId c = *app.selectedNodeId();
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
  CHECK(destroyNode(app, spare) && !app.selectedNodeId());
  CHECK(selectNode(app, mask) && selectedNode(app)->id == mask && !selectedNode(app)->processor);
  CHECK(destroyNode(app, mask) && !app.selectedNodeId());
  CHECK(ui::projectNodeList(view(app)).other.empty() && ui::projectNodeList(view(app)).spine == rows.spine);
  CHECK(graph::setDocumentOutput(app, image(app.primarySource)).ok());
  const auto sourceOnly = ui::projectNodeList(view(app));
  CHECK(sourceOnly.spine.empty() && sourceOnly.other.size() == 3 && selectNode(app, b));
  CHECK(!destroyNode(app, app.primarySource) && app.selectedNodeId() == b);
  return true;
}

static bool testIncompleteAndNonUnaryProjection() {
  App app;
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId();
  CHECK(addNativeCstNode(app)); const NodeId b = *app.selectedNodeId();
  CHECK(graph::disconnect(app, input(a)).ok());
  std::reverse(app.nodes.begin(), app.nodes.end());
  auto rows = ui::projectNodeList(view(app));
  CHECK(!rows.spineError.empty() && rows.spine == std::vector<NodeId>({a, b}));
  CHECK(selectNode(app, a) && selectedNode(app)->processor);
  auto before = topology(app); const auto epoch = RenderRuntimeTestAccess::snapshot(app.renderer).epoch;
  CHECK(!moveNode(app, b, a) && topology(app) == before);
  CHECK(RenderRuntimeTestAccess::snapshot(app.renderer).epoch == epoch);
  CHECK(destroyNode(app, a) && !app.selectedNodeId() && !findNode(app, b)->fixedInputs.at(ports::image));
  CHECK(selectNode(app, b)); before = topology(app);
  CHECK(!destroyNode(app, b) && app.selectedNodeId() == b && topology(app) == before);
  CHECK(app.getStatus().find("explicit replacement") != std::string::npos);

  App mixed;
  CHECK(addNativeExposureNode(mixed)); const NodeId upstream = *mixed.selectedNodeId();
  const NodeId mixer{"mixer"};
  CHECK(add(mixed, mixer, NodeOperation::TestSlotted));
  const auto slot = graph::addSlot(mixed, mixer).addedSlot; CHECK(slot);
  CHECK(graph::connect(mixed, image(upstream), {mixer, slot, ports::image}).ok());
  CHECK(graph::setDocumentOutput(mixed, image(mixer)).ok());
  selectOutput(mixed);
  CHECK(addNativeExposureNode(mixed)); const NodeId tail = *mixed.selectedNodeId();
  CHECK(addNativeCstNode(mixed)); const NodeId end = *mixed.selectedNodeId();
  std::reverse(mixed.nodes.begin(), mixed.nodes.end());
  rows = ui::projectNodeList(view(mixed));
  CHECK(!rows.spineError.empty() && rows.spine == std::vector<NodeId>({tail, end}));
  CHECK(rows.other.size() == 2 && std::find(rows.other.begin(), rows.other.end(), mixer) != rows.other.end());
  CHECK(selectNode(mixed, mixer) && selectedNode(mixed)->operation == NodeOperation::TestSlotted);
  before = topology(mixed);
  CHECK(!moveNode(mixed, end, tail) && topology(mixed) == before);
  CHECK(destroyNode(mixed, mixer) && !mixed.selectedNodeId() && !findNode(mixed, tail)->fixedInputs.at(ports::image));
  CHECK(ui::projectNodeList(view(mixed)).spine == rows.spine);
  return true;
}

static bool testRejectedTopologyEdits() {
  App app;
  CHECK(addNativeExposureNode(app)); const NodeId a = *app.selectedNodeId();
  const NodeId b{"masked-consumer"}, mask{"reference-to-a"};
  CHECK(add(app, b, NodeOperation::TestMaskedImage) && add(app, mask, NodeOperation::TestReferenceMask));
  CHECK(graph::connect(app, image(a), input(b)).ok());
  CHECK(graph::connect(app, image(a), input(mask, ports::reference)).ok());
  CHECK(graph::connect(app, {mask, ports::mask}, input(b, ports::mask)).ok());
  CHECK(graph::setDocumentOutput(app, image(b)).ok() && selectNode(app, a));
  const auto before = topology(app);
  CHECK(ui::projectNodeList(view(app)).spineError.empty());
  CHECK(!moveNode(app, a, b) && topology(app) == before && app.selectedNodeId() == a);
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
  CHECK(app.nodes.size() == 3 && app.selectedNodeId() == app.nodes[2].id);
  CHECK(selectedNode(app)->storedIdentifier == "missing.second");
  const NodeId migrated = *app.selectedNodeId();
  std::reverse(app.nodes.begin(), app.nodes.end());
  CHECK(app.selectedNodeId() == migrated && selectedNode(app)->storedIdentifier == "missing.second");

  PersistChain chain; chain.nodes = {persisted("first", 1.25), persisted("second", -2.0)};
  chain.selectedNodeId = "second";
  CHECK(saveInputSidecar(files.path("v2.png"), {}, chain));
  CHECK(loadSidecarFile(inputSidecarPath(files.path("v2.png")), loaded));
  applyChain(app, loaded.chain);
  CHECK(app.selectedNodeId() == NodeId{"second"} && exposure(selectedNode(app)) == -2.0);
  CHECK(moveNode(app, NodeId{"second"}, NodeId{"first"}));
  std::reverse(app.nodes.begin(), app.nodes.end());
  const auto captured = captureChain(app);
  CHECK(captured.selectedNodeId == "second" && captured.nodes.front().id == "second");
  std::vector<std::string> warnings;
  const auto fragment = captureFragment(app, {*app.selectedNodeId()}, warnings);
  std::string json, error; PersistTransfer decoded;
  CHECK(encodeTransferPayload(fragment, json, error) && decodeTransferPayload(json, decoded, error));
  CHECK(pasteFragment(app, std::get<PersistFragment>(decoded), app.selectedNodeId()));
  const NodeId pasted = *app.selectedNodeId();
  CHECK(pasted != NodeId{"second"} && exposure(selectedNode(app)) == -2.0);
  CHECK(findNode(app, pasted)->fixedInputs.at(ports::image) == image(NodeId{"second"}));
  chain.selectedNodeId = "missing"; chain.selectedNode = 1;
  applyChain(app, chain); CHECK(app.selectedNodeId() == NodeId{"second"}); // Legacy ordinal fallback.
  chain.selectedNode = 99;
  applyChain(app, chain); CHECK(app.selectedNodeId() == NodeId{"first"}); // Historical default selection.
  return true;
}

static bool testPhotoAndDocumentReplacement() {
  Files files;
  const auto photoA = files.path("a.png"), photoB = files.path("b.png");
  const ColorEncoding encoding{RgbGamut::Rec709, TransferFunction::Linear};
  CHECK(writeImage(raster(0.25f), photoA, encoding, 92) && writeImage(raster(0.5f), photoB, encoding, 92));
  App app; openPath(app, photoA);
  PersistChain chain; chain.nodes = {persisted("shared-id", 1.0)}; chain.selectedNodeId = "shared-id";
  applyChain(app, chain);
  app.paramFilter[0] = 'x';
  app.renderer.requestInspection(image(NodeId{"shared-id"})); app.renderer.start(); CHECK(idle(app));
  openPath(app, photoB); CHECK(idle(app));
  CHECK(!app.selectedNodeId() && app.paramFilter[0] == '\0' && publishedEndpoint(app) == image(app.primarySource));
  openPath(app, photoA); CHECK(idle(app));
  CHECK(!app.selectedNodeId() && exposure(findNode(app, NodeId{"shared-id"})) == 1.0);

  PersistChain replacement;
  replacement.nodes = {persisted("shared-id", 4.0), persisted("new-output")};
  replacement.selectedNodeId = "shared-id";
  App incoming; applyChain(incoming, replacement);
  PersistGrade grade; grade.graph = captureGraph({incoming.nodes, incoming.primarySource, incoming.documentOutput});
  grade.color = captureGradeColor(incoming); std::string error;
  CHECK(updateSidecarGrade(inputSidecarPath(photoB), grade, error));
  app.renderer.requestInspection(image(NodeId{"shared-id"})); CHECK(idle(app));
  openPath(app, photoB); CHECK(idle(app));
  CHECK(!app.selectedNodeId() && exposure(findNode(app, NodeId{"shared-id"})) == 4.0 && app.paramFilter[0] == '\0');
  CHECK(publishedEndpoint(app) == image(NodeId{"new-output"})); // Old ID exists, but old inspection is gone.

  app.renderer.requestInspection(image(NodeId{"shared-id"})); CHECK(idle(app));
  replacement.nodes[0].paramsJson["exposure"] = "-3.0";
  replacement.selectedNodeId = "new-output";
  applyChain(app, replacement); CHECK(idle(app));
  CHECK(app.selectedNodeId() == NodeId{"new-output"} && exposure(selectedNode(app)) == 0.0);
  CHECK(exposure(findNode(app, NodeId{"shared-id"})) == -3.0 && publishedEndpoint(app) == image(NodeId{"new-output"}));

  CHECK(selectNode(app, NodeId{"shared-id"}));
  app.renderer.requestInspection(image(NodeId{"shared-id"})); CHECK(idle(app));
  openPath(app, photoA); CHECK(idle(app));
  CHECK(!app.selectedNodeId() && publishedEndpoint(app) == image(NodeId{"shared-id"}));
  applyChain(app, {}); CHECK(idle(app));
  CHECK(!app.selectedNodeId() && !selectedNode(app) && publishedEndpoint(app) == image(app.primarySource));
  return true;
}

}  // namespace

bool testNodeList() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"Selection-aware Add Processor: serial bindings, Source-only, fan-out, Slots and repeated selection", testSelectionAwareInsertion},
      {"Off-chain Add Processor branches without changing Output or existing consumers", testOffChainInsertion},
      {"Invalid Add Processor selection preserves topology, selection and runtime demand", testInvalidInsertionSelection},
      {"Over-depth Add Processor rejects before backend construction or mutation", testInsertionDepthRefusal},
      {"Selection-aware insertion saves/reopens topology, identities and parameters", testInsertionSaveReopen},
      {"6E.1 NodeId selection, storage/topology reorder and logical parameter association", testSelectionIdentity},
      {"6E.1 selected/unselected deletion clears only stale selection and inspection", testDeletionAndInspection},
      {"6E.1 topology List View keeps off-spine nodes visible, selectable and removable", testProjectionAndOtherNodes},
      {"6E.1 incomplete/non-unary List View retains nodes without guessed rewiring", testIncompleteAndNonUnaryProjection},
      {"6E.1 unsupported/cyclic UI reorders and graph edits preserve topology", testRejectedTopologyEdits},
      {"6E.1 historical V1/V2 selection and graph-native fragment paste by NodeId", testLegacySelectionAndTransfers},
      {"6E.1 photo/document replacement resets old selection and inspection", testPhotoAndDocumentReplacement},
  };
  for (const auto &test : cases) {
    if (!test.test()) { std::fprintf(stderr, "selftest FAILED: %s\n", test.name); return false; }
    std::printf("ok  %s\n", test.name);
  }
  return true;
}
