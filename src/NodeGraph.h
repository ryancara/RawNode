#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

// App::nodes is the single graph storage, including Source. Execution follows
// topology; these lookups never interpret storage position as an ID.
Node *findNode(App &app, const NodeId &id);
const Node *findNode(const App &app, const NodeId &id);
const PortDeclaration *findOutput(const App &app, const OutputRef &output);
const std::optional<OutputRef> *findInput(const App &app, const InputAddress &input);
// Structural validation includes all stored edges/types/cycles. Disconnected
// required inputs remain representable; see requestedGraphError for readiness.
// Empty means valid; otherwise the first invariant violation is returned.
std::string documentStateError(const App &app);
bool requireEditableGraph(App &app);
// Control-thread processor edits retain backend synchronization and export
// exclusion. Accepted edits request processing at the current preview endpoint;
// ordinary edits do not drain active preview execution.
bool setNodeParameterValue(App &app, const NodeId &node, const std::string &parameter, const ParameterValue &value);
bool resetNodeParameter(App &app, const NodeId &node, const std::string &parameter);
bool activateNodeParameter(App &app, const NodeId &node, const std::string &parameter);
// Node-owned Strength uses DocumentMutation so planning/execution see one value.
// Like parameter edits, export/protection refuse it; history records gesture completion.
bool setNodeStrength(App &app, const NodeId &node, double strength);

// Selection is view-only. Resolve identity at use time; never retain a node
// pointer across a graph edit. Source selection exposes input settings, never
// a processing node through selectedNode().
Node *selectedNode(App &app);
const Node *selectedNode(const App &app);
// Source is selectable; selectedNode() deliberately returns only non-Source nodes.
bool selectNode(App &app, std::optional<NodeId> id);
void selectOutput(App &app);
std::string nodeDisplayName(const Node &node);
// Structural operations run on the single control thread. Each owns execution
// safety and its final preview decision; callers need no renderer protocol.
void clearNodes(App &app);
bool addNode(App &app, int pluginIndex);  // OFX plugin
bool addNativeExposureNode(App &app);
bool addNativeCstNode(App &app);
bool addCtlNode(App &app, const std::string &path);
bool destroyNode(App &app, const NodeId &id);
bool moveNode(App &app, const NodeId &from, const NodeId &to);
void setNodeEnabled(App &app, const NodeId &id, bool enabled);

// Historical V2 sidecar support only; live capture uses captureGraph/captureGrade.
// Refuse before writing when V2 cannot represent the topology.
// Caller owns control-thread lifetime and processor execution exclusion.
PersistChain captureChain(const App &app);

// Single-node convenience uses the same graph-native fragment batch splice.
// Group expansion is transient and external inputs are detached.
bool appendPersistedNode(App &app, const PersistNode &node, std::optional<NodeId> insertAfter = std::nullopt);
void applyChain(App &app, const PersistChain &chain);

// Fragment capture preserves arbitrary supported internal topology, detaches
// external inputs and reports omitted Mask/Reference links. No paste eligibility
// check is made here. May synchronise private state into parameters. Caller owns
// control-thread lifetime and processor execution exclusion; document/UI
// callers use captureNodeFragment() in persist/DocumentActions.h.
PersistFragment captureFragment(const App &app, const std::vector<NodeId> &ids,
                                std::vector<std::string> &warnings);
// Append a complete linear grade, or paste a compatible detached linear fragment.
// These own reconstruction, fresh identity/position remapping and one mutation.
bool appendGrade(App &app, const PersistGrade &grade, std::optional<NodeId> after = std::nullopt);
bool pasteFragment(App &app, const PersistFragment &fragment, std::optional<NodeId> after = std::nullopt);

// Capture all stored nodes, synchronising private state before reading parameters.
// Caller owns control-thread lifetime and graph/processor execution exclusion;
// these worker-free helpers never schedule or publish a live document.
PersistGraph captureGraph(GraphView graph);

namespace document_detail {
struct RestoredGraph {
  std::vector<Node> nodes;
  NodeId primarySource;
  OutputRef output{NodeId{}, ports::image};
};
// Construct a detached graph, or throw on invalid structure or installed
// parameter values. Does not publish a document or schedule rendering.
// History supplies complete availability by processor NodeId. Working instances
// must reconstruct with the recorded identity; missing instances stay missing.
// Ordinary sidecars/presets omit it and retain their missing-plugin policy.
RestoredGraph restoreGraph(const PersistGraph &graph,
                           const std::map<std::string, bool> *workingProcessors = nullptr);

// Operation-local restoration, so grade application can prepare nodes and
// colour/source changes before replacing anything. Both helpers borrow their
// document operation's DocumentMutation; neither schedules preview work.
struct RestoredChain {
  std::vector<Node> nodes;
  OutputRef output{NodeId{}, ports::image};
  std::optional<NodeId> selection;
};
RestoredChain restoreChain(App &app, const PersistChain &chain, bool strictParameters = false);
void replaceChain(App &app, RestoredChain &&chain);
}  // namespace document_detail

bool hasUnknownProcessorChoiceIds(const App &app);
