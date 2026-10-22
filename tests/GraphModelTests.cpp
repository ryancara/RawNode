#include "GraphModelTests.h"

#include "NodeGraph.h"
#include "GraphCommands.h"
#include "persist/DocumentActions.h"

#include <algorithm>
#include <cstdio>
#include <type_traits>

static_assert(!std::is_convertible_v<size_t, NodeId>);
static_assert(!std::is_constructible_v<NodeId, size_t>);
static_assert(!std::is_convertible_v<size_t, SlotId>);
static_assert(!std::is_convertible_v<SlotId, size_t>);
static_assert(!std::is_convertible_v<SlotId, NodeId>);
static_assert(!std::is_convertible_v<std::string, NodeId>);
static_assert(!std::is_convertible_v<PortId, NodeId>);
static_assert(!std::is_convertible_v<InputAddress, OutputRef>);

static bool testDocumentIdentity() {
  App app;
  const NodeId sourceId = app.primarySource;
  const OutputRef sourceImage{sourceId, ports::image};
  if (app.nodes.size() != 1 || app.nodes[0].id != sourceId ||
      app.nodes[0].operation != NodeOperation::Source ||
      app.documentOutput != sourceImage || !documentStateError(app).empty()) return false;
  const Node *source = findNode(app, sourceId);
  if (!source || source->processor || source->isMissingProcessor() ||
      !source->fixedInputs.empty() || !source->slots.empty()) return false;

  PersistFragment fragment;
  std::vector<std::string> warnings;
  if (!selectNode(app, app.primarySource) || app.selectedNodeId() != sourceId) return false;
  if (selectedNode(app) || captureNodeFragment(app, {app.nodes[0].id}, fragment, warnings) ||
      !captureChain(app).nodes.empty()) return false;
  destroyNode(app, app.nodes[0].id);
  setNodeEnabled(app, app.nodes[0].id, false);
  clearNodes(app);
  applyChain(app, {});
  if (app.nodes.size() != 1 || !app.nodes[0].enabled || app.primarySource != sourceId ||
      app.documentOutput != sourceImage || !documentStateError(app).empty()) return false;

  if (!addNativeExposureNode(app) || !addNativeCstNode(app)) return false;
  const NodeId first = app.nodes[1].id, second = app.nodes[2].id;
  if (first == second || first == sourceId || second == sourceId) return false;
  app.documentOutput = {first, ports::image};
  moveNode(app, app.nodes[2].id, app.nodes[1].id);
  // An off-spine node cannot be moved by the current spine controls.
  if (app.nodes[1].id != first || app.nodes[2].id != second ||
      app.documentOutput != OutputRef{first, ports::image}) return false;
  // Move Source too, proving identity/output lookups do not depend on position.
  std::reverse(app.nodes.begin(), app.nodes.end());
  if (findNode(app, first) != &app.nodes[1] || !findNode(app, second) ||
      !findOutput(app, app.documentOutput) || !documentStateError(app).empty()) return false;
  destroyNode(app, app.nodes[2].id);  // Source is now at the end.
  if (app.nodes.size() != 3 || app.documentOutput != OutputRef{first, ports::image}) return false;
  destroyNode(app, app.nodes[1].id);  // Splice deletion also rewires the stored downstream node.
  if (findNode(app, first) || app.documentOutput != sourceImage ||
      !documentStateError(app).empty()) return false;
  app.documentOutput = {second, ports::image}; // Capture the remaining complete linear graph.
  app.nodePositions.clear(); // Historical V2 fixtures intentionally omit layout.
  const PersistChain chain = captureChain(app);
  clearNodes(app);
  if (app.nodes.size() != 1 || app.primarySource != sourceId || app.documentOutput != sourceImage) return false;
  applyChain(app, chain);
  if (app.nodes.size() != 2 || app.primarySource != sourceId || !documentStateError(app).empty()) return false;
  if (!appendPersistedNode(app, chain.nodes[0]) || app.nodes.size() != 3 ||
      app.nodes[1].id == app.nodes[2].id) return false;

  // V2 copy/paste/presets contain processor records only. A colliding ID cannot
  // claim Source identity, even when restoring a foreign/unavailable processor.
  PersistNode record;
  record.id = sourceId.value;
  record.backend = "graph";
  record.identifier = "source";
  if (!appendPersistedNode(app, record)) return false;
  PersistChain foreign;
  foreign.nodes.push_back(record);
  applyChain(app, foreign);
  return app.nodes.size() == 2 && app.nodes[0].operation == NodeOperation::Source &&
         app.nodes[1].operation == NodeOperation::ImageProcessor && app.nodes[1].isMissingProcessor() &&
         app.nodes[1].id != sourceId && app.documentOutput == OutputRef{app.nodes[1].id, ports::image} && documentStateError(app).empty();
}

static bool testPortsAndReferences() {
  using Direction = PortDirection;
  const auto &source = nodeInterface(NodeOperation::Source);
  const auto *sourceOutput = source.findFixedPort(ports::image, Direction::Output);
  if (source.fixedPorts.size() != 1 || !source.slotInputs.empty() || !sourceOutput ||
      sourceOutput->type != ValueType::Image || source.findFixedPort(ports::image, Direction::Input)) return false;
  const auto &processor = nodeInterface(NodeOperation::ImageProcessor);
  const auto *imageIn = processor.findFixedPort(ports::image, Direction::Input);
  const auto *imageOut = processor.findFixedPort(ports::image, Direction::Output);
  if (!imageIn || !imageOut || imageIn == imageOut || !imageIn->required ||
      imageIn->type != ValueType::Image || imageOut->type != ValueType::Image ||
      imageIn->direction != Direction::Input || imageOut->direction != Direction::Output) return false;
  const auto &imageToMask = nodeInterface(NodeOperation::TestImageToMask);
  const auto &maskProcessor = nodeInterface(NodeOperation::TestMaskProcessor);
  const auto *analysisIn = imageToMask.findFixedPort(ports::image, Direction::Input);
  const auto *analysisOut = imageToMask.findFixedPort(ports::mask, Direction::Output);
  const auto *maskIn = maskProcessor.findFixedPort(ports::mask, Direction::Input);
  const auto *maskOut = maskProcessor.findFixedPort(ports::mask, Direction::Output);
  if (!analysisIn || !analysisOut || !maskIn || !maskOut || !analysisIn->required || !maskIn->required ||
      analysisIn->type != ValueType::Image || analysisOut->type != ValueType::Mask ||
      maskIn->type != ValueType::Mask || maskOut->type != ValueType::Mask) return false;
  if (maskProcessor.findFixedPort(ports::image, Direction::Input) ||
      processor.findFixedPort(PortId{"unknown"}, Direction::Output)) return false;

  App app;
  Node multi(NodeOperation::TestMultipleOutputs);
  multi.id = NodeId{"multiple"};
  app.nodes.push_back(std::move(multi));
  const OutputRef image{NodeId{"multiple"}, ports::image};
  const OutputRef original{NodeId{"multiple"}, PortId{"original"}};
  const OutputRef mask{NodeId{"multiple"}, ports::mask};
  if (image == original || image == mask || original == mask || app.nodes.back().isMissingProcessor()) return false;
  for (const auto &output : {image, original}) {
    const auto *port = findOutput(app, output);
    if (!port || port->type != ValueType::Image || port->direction != Direction::Output) return false;
    app.documentOutput = output;
    if (!documentStateError(app).empty()) return false;
  }
  if (findOutput(app, mask)->type != ValueType::Mask) return false;
  app.documentOutput = mask;
  if (documentStateError(app).empty()) return false;
  app.documentOutput = {NodeId{"missing"}, ports::image};
  if (documentStateError(app).empty()) return false;
  app.documentOutput = {app.primarySource, ports::mask};
  if (documentStateError(app).empty()) return false;
  app.documentOutput = original;

  const InputAddress input{image.nodeId, std::nullopt, ports::image};
  const auto *binding = findInput(app, input);
  if (!binding || *binding || findInput(app, {image.nodeId, std::nullopt, ports::mask}) ||
      findInput(app, {app.primarySource, std::nullopt, ports::image})) return false;
  const OutputRef producer{app.primarySource, ports::image};
  app.nodes[1].fixedInputs.at(ports::image) = producer;
  Node consumer;
  consumer.id = NodeId{"consumer"};
  consumer.fixedInputs.at(ports::image) = producer;
  app.nodes.push_back(std::move(consumer));
  return *findInput(app, input) == producer &&
         *findInput(app, {NodeId{"consumer"}, std::nullopt, ports::image}) == producer &&
         app.nodes.back().isMissingProcessor() && documentStateError(app).empty();
}

static bool testSlotIdentity() {
  App app;
  Node node(NodeOperation::TestSlotted);
  node.id = NodeId{"slotted"};
  for (int i = 0; i < 3; ++i) {
    Node producer(NodeOperation::TestReferenceMask);
    producer.id = NodeId{"mask-" + std::to_string(i + 1)};
    if (!graph::addNode(app, std::move(producer)).ok()) return false;
    Slot slot = node.makeSlot();
    slot.inputs.at(ports::image) = OutputRef{app.primarySource, ports::image};
    slot.inputs.at(ports::mask) = OutputRef{NodeId{"mask-" + std::to_string(slot.id.value)}, ports::mask};
    slot.settings.enabled = slot.id != SlotId{1};
    node.slots.push_back(std::move(slot));
  }
  std::rotate(node.slots.begin(), node.slots.begin() + 2, node.slots.end());
  if (node.slots[0].id != SlotId{3} || node.slots[1].id != SlotId{1} || node.slots[2].id != SlotId{2}) return false;
  const auto intact = [&]() {
    for (const auto &slot : node.slots) {
      if (slot.inputs.at(ports::image) != OutputRef{app.primarySource, ports::image} ||
          slot.inputs.at(ports::mask) != OutputRef{NodeId{"mask-" + std::to_string(slot.id.value)}, ports::mask} ||
          slot.settings.enabled != (slot.id != SlotId{1})) return false;
    }
    return true;
  };
  if (!intact() || node.findSlot(SlotId{1}) != &node.slots[1] || node.findSlot(SlotId{0})) return false;
  std::swap(node.slots[0], node.slots[2]);
  if (node.slots[0].id != SlotId{2} || node.slots[1].id != SlotId{1} || node.slots[2].id != SlotId{3} || !intact()) return false;
  Slot inserted = node.makeSlot();
  if (inserted.id != SlotId{4}) return false;
  node.slots.insert(node.slots.begin() + 1, std::move(inserted));
  if (node.slots[0].id != SlotId{2} || node.slots[1].id != SlotId{4} ||
      node.slots[2].id != SlotId{1} || node.slots[3].id != SlotId{3}) return false;
  node.slots.erase(node.slots.begin() + 1);
  if (!intact() || node.slots[0].id != SlotId{2} || node.slots[1].id != SlotId{1} || node.slots[2].id != SlotId{3}) return false;
  node.slots.erase(node.slots.begin() + 2);  // Delete highest surviving ID too.
  if (node.makeSlot().id != SlotId{5} || !intact()) return false;
  Node other(NodeOperation::TestSlotted);
  if (other.makeSlot().id != SlotId{1}) return false;  // IDs are node-local.
  app.nodes.push_back(std::move(node));
  const InputAddress slotInput{NodeId{"slotted"}, SlotId{1}, ports::mask};
  const auto *binding = findInput(app, slotInput);
  if (!binding || *binding != OutputRef{NodeId{"mask-1"}, ports::mask} ||
      findInput(app, {slotInput.nodeId, SlotId{99}, ports::mask}) ||
      findInput(app, {slotInput.nodeId, SlotId{1}, ports::reference})) return false;
  const auto &interface = nodeInterface(NodeOperation::TestSlotted);
  const auto *reference = interface.findFixedPort(ports::reference, PortDirection::Input);
  const auto *slotImage = interface.findSlotInput(ports::image);
  const auto *slotMask = interface.findSlotInput(ports::mask);
  return reference && !reference->required && reference->type == ValueType::Image &&
         slotImage && slotImage->required && slotImage->direction == PortDirection::Input &&
         slotMask && !slotMask->required && slotMask->type == ValueType::Mask && documentStateError(app).empty();
}

static bool testInvalidIdentities() {
  App app;
  Node node(NodeOperation::TestSlotted);
  node.id = app.primarySource;
  app.nodes.push_back(std::move(node));
  if (documentStateError(app) != "Duplicate NodeId") return false;
  Node &slotted = app.nodes.back();
  slotted.id = NodeId{"slotted"};
  slotted.slots.push_back(slotted.makeSlot());
  slotted.slots.push_back(slotted.makeSlot());
  slotted.slots[1].id = slotted.slots[0].id;
  if (documentStateError(app) != "Duplicate SlotId") return false;
  slotted.slots[1].id = SlotId{2};
  if (!documentStateError(app).empty()) return false;
  slotted.slots[1].id = SlotId{0};
  if (documentStateError(app).empty()) return false;
  slotted.slots[1].id = SlotId{2};
  slotted.fixedInputs.emplace(PortId{"unknown"}, std::nullopt);
  if (documentStateError(app).empty()) return false;
  slotted.fixedInputs.erase(PortId{"unknown"});
  slotted.slots[1].inputs.erase(ports::image);
  if (documentStateError(app).empty()) return false;
  slotted.slots[1].inputs.emplace(ports::image, std::nullopt);
  Node secondSource(NodeOperation::Source);
  secondSource.id = NodeId{"second-source"};
  app.nodes.push_back(std::move(secondSource));
  if (documentStateError(app).empty()) return false;
  app.nodes.pop_back();
  app.primarySource = NodeId{"missing-source"};
  if (documentStateError(app).empty()) return false;
  app.primarySource = NodeId{"source"};
  app.nodes[0].fixedInputs.emplace(ports::image, std::nullopt);
  if (documentStateError(app).empty()) return false;
  app.nodes[0].fixedInputs.clear();
  app.nodes.erase(app.nodes.begin());
  return !documentStateError(app).empty();
}

bool testGraphModel() {
  const struct { const char *name; bool (*test)(); } cases[] = {
      {"6A Source, stable Node IDs and explicit document output", testDocumentIdentity},
      {"6A static typed ports, direction and multiple output identity", testPortsAndReferences},
      {"6A ordered Slots [3,1,2], settings/references and non-reused IDs", testSlotIdentity},
      {"6A duplicate identities and invalid document state", testInvalidIdentities},
  };
  for (const auto &test : cases) {
    if (!test.test()) {
      std::fprintf(stderr, "selftest FAILED: %s\n", test.name);
      return false;
    }
    std::printf("ok  %s\n", test.name);
  }
  return true;
}
