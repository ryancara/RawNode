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
#include "ui/GraphLayout.h"

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

// Helpers borrow their public operation's exclusion scope. Capture may flush
// processor-private state before translating parameters to persistence records.
static void clearNodesWithinMutation(App &app);
// Resolved from the shared selection before processor construction/mutation.
// An empty target appends at Output; a branch leaves all consumers untouched.
struct ProcessorInsertion {
  std::optional<InputAddress> before;
  std::optional<OutputRef> branch;
};
static std::optional<ProcessorInsertion> processorInsertion(App &app);
static bool insertProcessorNode(App &app, std::unique_ptr<Processor> processor, const ProcessorInsertion &insertion);
static bool addOfxWithinMutation(App &app, int pluginIndex, const ProcessorInsertion &insertion);
static bool addCtlWithinMutation(App &app, const std::string &path, const ProcessorInsertion &insertion);
static Node makeProcessorNode(const std::vector<Node> &nodes, std::unique_ptr<Processor> processor);
static NodeId makeNodeId(const std::vector<Node> &nodes);

static PersistNode capturePersistedNode(const Node &node, bool graphNative = false);
static std::vector<NodeId> linearCaptureOrder(const App &app);
static Node restoredNode(const std::vector<Node> &nodes, const PersistNode &persisted, bool preserveId,
                         bool graphNative = false, bool keepMissing = false);
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
  Node *node = app.selectedNodeId() ? findNode(app, *app.selectedNodeId()) : nullptr;
  return node && node->operation != NodeOperation::Source ? node : nullptr;
}

const Node *selectedNode(const App &app) {
  const Node *node = app.selectedNodeId() ? findNode(app, *app.selectedNodeId()) : nullptr;
  return node && node->operation != NodeOperation::Source ? node : nullptr;
}

bool selectNode(App &app, std::optional<NodeId> id) {
  if (id && !findNode(app, *id)) return false;
  InspectorSelection selection;
  if (id) selection = *id;
  if (app.selection != selection) {
    app.selection = std::move(selection);
    app.paramFilter[0] = '\0';
  }
  return true;
}

void selectOutput(App &app) {
  if (!app.outputSelected()) {
    app.selection = InspectorEndpoint::Output;
    app.paramFilter[0] = '\0';
  }
}

std::string nodeDisplayName(const Node &node) {
  if (node.operation == NodeOperation::Source) return "Source";
  if (node.operation != NodeOperation::ImageProcessor) return "Test graph operation";
  if (node.processor) return node.processor->displayName();
  if (!node.storedLabel.empty()) return node.storedLabel + " (Missing)";
  if (!node.storedIdentifier.empty()) return node.storedIdentifier + " (Missing)";
  return "Missing processor";
}

bool requireEditableGraph(App &app) {
  if (!app.isPhotoProtected()) return true;
  app.setStatus("The saved grade was not loaded; editing is disabled");
  return false;
}

template<class Edit>
static bool editNodeParameter(App &app, const NodeId &id, Edit edit) {
  if (!requireEditableGraph(app) || !app.renderer.canEditParameters()) return false;
  Node *node = findNode(app, id);
  if (!node || !node->processor || !edit(*node->processor)) return false;
  app.history.noteUnrecordedEdit();
  app.renderer.requestPreview();
  return true;
}

bool setNodeParameterValue(App &app, const NodeId &id, const std::string &parameter, const ParameterValue &value) {
  return editNodeParameter(app, id, [&](Processor &processor) { return processor.setParameterValue(parameter, value); });
}
bool resetNodeParameter(App &app, const NodeId &id, const std::string &parameter) {
  return editNodeParameter(app, id, [&](Processor &processor) { return processor.resetParameter(parameter); });
}
bool activateNodeParameter(App &app, const NodeId &id, const std::string &parameter) {
  return editNodeParameter(app, id, [&](Processor &processor) { return processor.activateParameter(parameter); });
}

void clearNodes(App &app) {
  if (!requireEditableGraph(app)) return;
  if (app.nodes.size() == 1) {
    selectNode(app, std::nullopt);
    app.renderer.resetInspection();
    return;
  }
  {
    DocumentMutation mutation(app);
    clearNodesWithinMutation(app);
    mutation.changed();
    app.history.recordCompletedEdit(app, &mutation);
  }
}

bool addNode(App &app, int pluginIndex) {
  if (!requireEditableGraph(app)) return false;
  if (pluginIndex < 0 || pluginIndex >= (int)gPlugins.size()) return false;
  const auto insertion = processorInsertion(app);
  if (!insertion) return false;
  bool added;
  {
    DocumentMutation mutation(app);
    added = addOfxWithinMutation(app, pluginIndex, *insertion);
    if (added) {
      mutation.changed();
      app.history.recordCompletedEdit(app, &mutation);
    }
  }
  return added;
}

bool addNativeExposureNode(App &app) {
  if (!requireEditableGraph(app)) return false;
  const auto insertion = processorInsertion(app);
  if (!insertion) return false;
  bool added;
  {
    DocumentMutation mutation(app);
    added = insertProcessorNode(app, std::make_unique<NativeExposureProcessor>(), *insertion);
    if (added) {
      mutation.changed();
      app.history.recordCompletedEdit(app, &mutation);
    }
  }
  return added;
}

bool addNativeCstNode(App &app) {
  if (!requireEditableGraph(app)) return false;
  const auto insertion = processorInsertion(app);
  if (!insertion) return false;
  bool added;
  {
    DocumentMutation mutation(app);
    added = insertProcessorNode(app, std::make_unique<NativeCstProcessor>(), *insertion);
    if (added) {
      mutation.changed();
      app.history.recordCompletedEdit(app, &mutation);
    }
  }
  return added;
}

bool addCtlNode(App &app, const std::string &path) {
  if (!requireEditableGraph(app)) return false;
  const auto insertion = processorInsertion(app);
  if (!insertion) return false;
  bool added;
  {
    DocumentMutation mutation(app);
    added = addCtlWithinMutation(app, path, *insertion);
    if (added) {
      mutation.changed();
      app.history.recordCompletedEdit(app, &mutation);
    }
  }
  return added;
}

bool destroyNode(App &app, const NodeId &id) {
  if (!requireEditableGraph(app)) return false;
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
  if (!requireEditableGraph(app)) return false;
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
  if (!requireEditableGraph(app)) return;
  Node *node = findNode(app, id);
  if (!node || node->enabled == enabled || node->operation == NodeOperation::Source) return;
  {
    DocumentMutation mutation(app);
    node->enabled = enabled;
    mutation.changed();
    app.history.recordCompletedEdit(app, &mutation);
  }
}

static std::vector<NodeId> linearCaptureOrder(const App &app) {
  if (app.isPhotoProtected()) throw std::runtime_error("The saved grade was not loaded; capture is disabled");
  if (!app.nodePositions.empty()) throw std::runtime_error("Linear V2 transfers cannot preserve node positions");
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
  auto node = persisted;
  // A single-node attachment has no internal incoming links. In particular,
  // foreign IDs must never resolve to nodes already in this document.
  node.inputs = {{ports::image, std::nullopt}};
  return pasteFragment(app, PersistFragment{{std::move(node)}, {}}, std::move(insertAfter));
}

void applyChain(App &app, const PersistChain &chain) {
  if (!requireEditableGraph(app)) return;
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

PersistFragment captureFragment(const App &app, const std::vector<NodeId> &ids,
                                std::vector<std::string> &warnings) {
  if (app.isPhotoProtected()) throw std::runtime_error("The saved grade was not loaded; capture is disabled");
  PersistFragment fragment;
  std::vector<std::string> omitted;
  for (const auto &id : ids) {
    const Node *node = findNode(app, id);
    if (!node || node->operation == NodeOperation::Source) throw std::runtime_error("Cannot copy Source or a stale node");
    auto record = capturePersistedNode(*node, true);
    const auto detach = [&](InputBindings &bindings) {
      for (auto &binding : bindings) {
        if (!binding.second || std::find(ids.begin(), ids.end(), binding.second->nodeId) != ids.end()) continue;
        if (binding.first == ports::reference || binding.first == ports::mask)
          omitted.push_back("Omitted external " + binding.first.value + " connection on " + id.value);
        binding.second.reset();
      }
    };
    detach(record.inputs);
    for (auto &slot : record.slots) detach(slot.inputs);
    fragment.nodes.push_back(std::move(record));
    const auto position = app.nodePositions.find(id.value);
    if (position != app.nodePositions.end()) fragment.positions.emplace(*position);
  }
  std::string json, error;
  if (!persist_detail::encodeTransferPayload(fragment, json, error, true)) throw std::runtime_error(error);
  warnings = std::move(omitted);
  return fragment;
}

// Attachment eligibility is a topology-only check. Backend construction below
// consumes this derived order, never node-array order.
static std::vector<NodeId> linearFragmentOrder(const PersistFragment &fragment) {
  // Validate attachment using topology only, before constructing processors.
  // A synthetic Source binds only the fragment's one detached ordinary entry.
  auto record = PersistGraph{fragment.nodes, NodeId{"fragment-source"}, {NodeId{}, ports::image}};
  while (std::any_of(record.nodes.begin(), record.nodes.end(), [&](const auto &n) { return n.id == record.primarySource.value; }))
    record.primarySource.value += '-';
  PersistNode source; source.id = record.primarySource.value; source.operation = NodeOperation::Source;
  std::vector<Node> topology;
  std::optional<NodeId> entry;
  std::vector<NodeId> tails;
  for (auto &node : record.nodes) {
    if (node.operation != NodeOperation::ImageProcessor || !node.slots.empty() || node.inputs.size() != 1)
      throw std::runtime_error("Only ordinary linear fragments can be attached");
    auto &input = node.inputs.at(ports::image);
    if (!input) {
      if (entry) throw std::runtime_error("Fragment has multiple entries");
      entry = NodeId{node.id}; input = OutputRef{record.primarySource, ports::image};
    }
    bool consumed = false;
    for (const auto &consumer : fragment.nodes)
      if (consumer.inputs.at(ports::image) == std::optional<OutputRef>{{NodeId{node.id}, ports::image}}) consumed = true;
    if (!consumed) tails.push_back(NodeId{node.id});
  }
  if (!entry || tails.size() != 1) throw std::runtime_error("Fragment is not a complete linear chain");
  record.nodes.push_back(std::move(source));
  record.documentOutput = {tails.front(), ports::image};
  for (const auto &saved : record.nodes) {
    Node node(saved.operation); node.id = NodeId{saved.id}; node.fixedInputs = saved.inputs;
    topology.push_back(std::move(node));
  }
  std::vector<NodeId> order;
  const auto error = graph::imageSpine({topology, record.primarySource, record.documentOutput}, order);
  if (!error.empty() || order.size() != fragment.nodes.size())
    throw std::runtime_error("Fragment is not a complete linear chain");
  return order;
}

static bool insertFragment(App &app, const PersistFragment &fragment, std::optional<NodeId> after) {
  if (!requireEditableGraph(app)) return false;
  document_detail::DocumentMutation::assertControlThread(app);
  try {
    std::string json, error;
    if (!encodeTransferPayload(fragment, json, error)) throw std::runtime_error(error);
    const auto order = linearFragmentOrder(fragment);
    std::vector<NodeId> destinationSpine;
    error = graph::imageSpine({app.nodes, app.primarySource, app.documentOutput}, destinationSpine);
    if (!error.empty()) throw std::runtime_error(error);
    if (after && *after != app.primarySource && std::find(destinationSpine.begin(), destinationSpine.end(), *after) == destinationSpine.end())
      throw std::runtime_error("Insertion requires a current main Image spine node");
    std::map<std::string, NodeId> ids;
    for (const auto &id : order) {
      NodeId fresh;
      do { fresh = makeNodeId(app.nodes); }
      while (std::any_of(fragment.nodes.begin(), fragment.nodes.end(), [&](const auto &n) { return n.id == fresh.value; }));
      ids.emplace(id.value, std::move(fresh));
    }
    auto positions = app.nodePositions;
    for (const auto &position : fragment.positions) {
      const auto remap = ids.find(position.first);
      if (remap != ids.end()) positions.emplace(remap->second.value, position.second);
    }
    std::optional<NodeId> selection = ids.at(order.back().value);
    // A safe incoming fragment can still exceed the destination's depth. Run
    // the same batch command on backend-free topology before draining workers
    // for processor construction. This is local staging, never execution state.
    std::vector<Node> topology, pending;
    topology.reserve(app.nodes.size());
    for (const auto &live : app.nodes) {
      Node node(live.operation); node.id = live.id; node.enabled = live.enabled;
      node.fixedInputs = live.fixedInputs; node.slots = live.slots;
      topology.push_back(std::move(node));
    }
    for (const auto &id : order) {
      Node node;
      node.id = ids.at(id.value);
      if (!pending.empty()) node.fixedInputs.at(ports::image) = OutputRef{pending.back().id, ports::image};
      pending.push_back(std::move(node));
    }
    auto output = app.documentOutput;
    const auto staged = graph::insertImageChain({topology, app.primarySource, output}, pending, after);
    if (!staged.ok()) throw std::runtime_error(staged.error);
    {
      document_detail::DocumentMutation mutation(app);
      std::vector<Node> additions;
      additions.reserve(order.size());
      for (const auto &id : order) {
        const auto saved = std::find_if(fragment.nodes.begin(), fragment.nodes.end(), [&](const auto &n) { return n.id == id.value; });
        Node node = restoredNode(additions, *saved, true, true);
        node.id = ids.at(saved->id);
        node.fixedInputs = saved->inputs;
        for (auto &binding : node.fixedInputs) if (binding.second) binding.second->nodeId = ids.at(binding.second->nodeId.value);
        additions.push_back(std::move(node));
      }
      const auto result = graph::insertImageChain({app.nodes, app.primarySource, app.documentOutput}, additions, after);
      if (!result.ok()) throw std::runtime_error(result.error);
      app.nodePositions.swap(positions);
      selectNode(app, std::move(selection));
      mutation.changed();
      app.history.recordCompletedEdit(app, &mutation);
    }
    return true;
  } catch (const std::exception &failure) {
    app.setStatus("Could not insert grade/fragment: " + std::string(failure.what()));
    return false;
  }
}

bool pasteFragment(App &app, const PersistFragment &fragment, std::optional<NodeId> after) {
  return insertFragment(app, fragment, std::move(after));
}

bool appendGrade(App &app, const PersistGrade &grade, std::optional<NodeId> after) {
  if (!requireEditableGraph(app)) return false;
  try {
    PersistFile file; file.grade = grade;
    std::string json, error;
    if (!encodeGraphFile(file, json, error)) throw std::runtime_error(error);
    // All records (including off-spine nodes) participate in validation.
    std::vector<Node> topology;
    for (const auto &saved : grade.graph.nodes) {
      Node node(saved.operation); node.id = NodeId{saved.id}; node.fixedInputs = saved.inputs;
      node.slots = saved.slots;
      topology.push_back(std::move(node));
    }
    std::vector<NodeId> order;
    error = graph::imageSpine({topology, grade.graph.primarySource, grade.graph.documentOutput}, order);
    if (!error.empty() || order.size() + 1 != topology.size()) throw std::runtime_error("Append requires a complete ordinary linear grade");
    // Validate destination/selection even for the Source-only no-op.
    std::vector<NodeId> destination;
    error = graph::imageSpine({app.nodes, app.primarySource, app.documentOutput}, destination);
    if (!error.empty()) throw std::runtime_error(error);
    if (after && *after != app.primarySource && std::find(destination.begin(), destination.end(), *after) == destination.end())
      throw std::runtime_error("Insertion requires a current main Image spine node");
    if (order.empty()) return true;
    // Detach only Source.image; no auxiliary dependency can be omitted here.
    PersistFragment fragment;
    for (const auto &saved : grade.graph.nodes) if (saved.operation != NodeOperation::Source) {
      auto node = saved;
      for (auto &binding : node.inputs) if (binding.second && binding.second->nodeId == grade.graph.primarySource) {
        if (binding.first != ports::image || binding.second->outputPortId != ports::image)
          throw std::runtime_error("Append cannot attach auxiliary connections");
        binding.second.reset();
      }
      fragment.nodes.push_back(std::move(node));
      const auto position = grade.positions.find(saved.id);
      if (position != grade.positions.end()) fragment.positions.emplace(*position);
    }
    return insertFragment(app, fragment, std::move(after));
  } catch (const std::exception &failure) {
    app.setStatus("Could not append grade: " + std::string(failure.what()));
    return false;
  }
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
RestoredGraph restoreGraph(const PersistGraph &graph, const std::map<std::string, bool> *workingProcessors) {
  const auto error = persist_detail::graphRecordError(graph, true);
  if (!error.empty()) throw std::runtime_error(error);
  if (workingProcessors) {
    size_t processors = 0;
    for (const auto &record : graph.nodes) if (record.operation == NodeOperation::ImageProcessor) {
      ++processors;
      if (workingProcessors->find(record.id) == workingProcessors->end())
        throw std::runtime_error("Missing history processor availability: " + record.id);
    }
    if (processors != workingProcessors->size()) throw std::runtime_error("Invalid history processor availability");
  }
  RestoredGraph restored;
  restored.primarySource = graph.primarySource;
  restored.output = graph.documentOutput;
  restored.nodes.reserve(graph.nodes.size());
  // First restore identities and backend state. Bindings may refer forwards in
  // storage; no insertion/splice command may infer topology from this order.
  for (const auto &record : graph.nodes) {
    const bool keepMissing = workingProcessors && record.operation == NodeOperation::ImageProcessor &&
                             !workingProcessors->at(record.id);
    Node node = record.operation == NodeOperation::ImageProcessor
        ? restoredNode(restored.nodes, record, /*preserveId=*/true, /*graphNative=*/true, keepMissing) : Node(record.operation);
    if (workingProcessors && record.operation == NodeOperation::ImageProcessor && !keepMissing &&
        (!node.processor || processorBackendName(node.processor->backend()) != record.backend ||
         node.processor->identifier() != record.identifier))
      throw std::runtime_error("Could not reconstruct working processor: " + record.id + " (" +
                               record.backend + ": " + record.identifier + ")");
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

RestoredChain restoreChain(App &app, const PersistChain &chain, bool strictParameters) {
  RestoredChain restored;
  restored.nodes.reserve(chain.nodes.size() + 1);
  Node source(NodeOperation::Source);
  source.id = app.primarySource;
  restored.nodes.push_back(std::move(source));
  restored.output = {app.primarySource, ports::image};
  for (const PersistNode &persisted : chain.nodes) {
    Node node = restoredNode(restored.nodes, persisted, true, strictParameters);
    node.groupOpen = persisted.groupOpen;
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
  app.nodePositions.clear();
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
  app.nodePositions.clear();
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
static std::optional<ProcessorInsertion> processorInsertion(App &app) {
  DocumentMutation::assertControlThread(app);
  const auto reject = [&](const std::string &error) -> std::optional<ProcessorInsertion> {
    app.setStatus("Cannot add processor: " + error);
    return std::nullopt;
  };
  const auto validate = [&](ProcessorInsertion insertion) -> std::optional<ProcessorInsertion> {
    const auto result = graph::validateImageNodeInsertion(
        {app.nodes, app.primarySource, app.documentOutput}, insertion.before, insertion.branch);
    if (!result.ok()) return reject(result.error);
    return insertion;
  };
  const auto error = documentStateError(app);
  if (!error.empty()) return reject(error);
  const auto selected = app.selectedNodeId();
  if (!selected) return validate({}); // No selection or virtual Output.
  const OutputRef producer{*selected, ports::image};
  const auto *output = findOutput(app, producer);
  if (!output || output->type != ValueType::Image)
    return reject("Selected node has no compatible Image output");

  std::vector<NodeId> spine;
  const auto spineError = graph::imageSpine({app.nodes, app.primarySource, app.documentOutput}, spine);
  auto successor = spine.end();
  if (*selected == app.primarySource) {
    // A partial output-side spine cannot identify Source's main-chain successor.
    // Selected processors in that spine can still splice at a known successor.
    if (!spineError.empty()) return reject(spineError);
    successor = spine.begin();
  } else {
    successor = std::find(spine.begin(), spine.end(), *selected);
    if (successor == spine.end()) return validate({std::nullopt, producer});
    ++successor;
  }
  if (successor == spine.end()) return validate({});
  return validate({InputAddress{*successor, std::nullopt, ports::image}, std::nullopt});
}

static bool insertProcessorNode(App &app, std::unique_ptr<Processor> processor, const ProcessorInsertion &insertion) {
  if (!processor) return false;
  auto node = makeProcessorNode(app.nodes, std::move(processor));
  std::optional<NodeId> selection = node.id; // Allocate identity before committing the graph.
  const graph::Document document{app.nodes, app.primarySource, app.documentOutput};
  const auto upstream = insertion.branch ? *insertion.branch
      : insertion.before ? **findInput(document, *insertion.before) : app.documentOutput;
  auto positions = ui::positionsWithNewProcessor(document, app.nodePositions, node.id, upstream.nodeId);
  // Bind the detached branch before addNode stages/validates/commits it. Never
  // publish a disconnected node followed by a separately fallible connection.
  if (insertion.branch) node.fixedInputs.at(ports::image) = insertion.branch;
  const auto result = insertion.branch ? graph::addNode(document, std::move(node))
      : graph::insertImageNode(document, std::move(node), insertion.before);
  if (!result.ok()) { app.setStatus(result.error); return false; }
  app.nodePositions.swap(positions);
  selectNode(app, std::move(selection));
  return true;
}

static bool addOfxWithinMutation(App &app, int pluginIndex, const ProcessorInsertion &insertion) {
  auto processor = OfxProcessor::create(pluginIndex);
  if (!processor) {
    app.setStatus("Plugin failed to create an instance");
    return false;
  }
  return insertProcessorNode(app, std::move(processor), insertion);
}

static bool addCtlWithinMutation(App &app, const std::string &path, const ProcessorInsertion &insertion) {
  std::string error;
  auto processor = CtlProcessor::create(path, &error);
  if (!processor) {
    app.setStatus("Could not load CTL" + (error.empty() ? std::string() : ": " + error));
    return false;
  }
  return insertProcessorNode(app, std::move(processor), insertion);
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
    const auto synced = node.processor->syncPrivateData();
    if (!synced.ok) throw std::runtime_error(synced.message);
    persisted.backend = processorBackendName(node.processor->backend());
    persisted.identifier = node.processor->identifier();
    persisted.label = graphNative && !node.storedLabel.empty() ? node.storedLabel : node.processor->displayName();

    for (const ProcessorParameter &param : node.processor->parameters()) {
      if (!param.persistValue || !persistedParameterType(param.type)) {
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

static void restoreParameterValues(Processor &processor, const PersistNode &persisted, bool graphNative,
                                   bool validateChoiceMembership = true) {
  for (const auto &param : processor.parameters()) {
    const auto value = persisted.paramsJson.find(param.id);
    if (value == persisted.paramsJson.end()) continue;
    if (!graphNative) { applyParamValueJson(processor, param, value->second); continue; }
    if (!param.persistValue || !persistedParameterType(param.type))
      throw std::runtime_error("Non-persistent installed parameter: " + param.id);
    const auto parsed = persist_detail::parameterValue(param, value->second, validateChoiceMembership);
    if (!processor.setParameterValue(param.id, parsed, false))
      throw std::runtime_error("Processor refused parameter: " + param.id);
    bool matched = false;
    for (const auto &updated : processor.parameters())
      if (updated.id == param.id && updated.value == parsed) matched = true;
    if (!matched) throw std::runtime_error("Processor changed saved parameter value: " + param.id);
  }
}

static void validateRestoredChoices(const Processor &processor, const PersistNode &persisted) {
  for (const auto &param : processor.parameters()) {
    const auto saved = persisted.paramsJson.find(param.id);
    if (param.type != ParameterType::Choice || saved == persisted.paramsJson.end()) continue;
    if (!param.persistValue) throw std::runtime_error("Non-persistent installed parameter: " + param.id);
    // CreateInstance may populate options and legitimately change its selection.
    // Validate both values against the instance menu without restoring again.
    persist_detail::parameterValue(param, saved->second);
    persist_detail::parameterValue(param, paramValueJson(param));
  }
}

static Node restoredNode(const std::vector<Node> &nodes, const PersistNode &persisted, bool preserveId,
                         bool graphNative, bool keepMissing) {
  const std::string backend = persisted.backend.empty() ? "ofx" : persisted.backend;
  std::unique_ptr<Processor> processor;
  if (keepMissing) {
    // Replay the recorded absence even if this backend has since become available.
  } else if (backend == "ofx") {
    const int index = findPluginIndex(persisted.identifier, graphNative ? std::string() : persisted.label);
    if (index >= 0) processor = OfxProcessor::create(index, [&](Processor &prepared) {
      restoreParameterValues(prepared, persisted, graphNative, /*validateChoiceMembership=*/false);
    });
    if (processor && graphNative) validateRestoredChoices(*processor, persisted);
  } else if (backend == "native" && persisted.identifier == NativeExposureProcessor::kIdentifier) {
    processor = std::make_unique<NativeExposureProcessor>();
  } else if (backend == "native" && persisted.identifier == NativeCstProcessor::kIdentifier) {
    processor = std::make_unique<NativeCstProcessor>();
  } else if (backend == "ctl") {
    processor = CtlProcessor::create(persisted.identifier);
  }
  Node node = makeProcessorNode(nodes, std::move(processor));
  if (graphNative && preserveId) node.id = NodeId{persisted.id};
  else if (preserveId) node.id = restoredNodeId(nodes, persisted.id);
  node.enabled = persisted.enabled;
  if (!graphNative) node.groupOpen = persisted.groupOpen;
  node.storedBackend = backend;
  node.storedIdentifier = persisted.identifier;
  node.storedLabel = persisted.label;
  node.preservedParamsJson = persisted.paramsJson;
  if (node.processor && backend != "ofx") restoreParameterValues(*node.processor, persisted, graphNative);
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
