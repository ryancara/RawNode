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

// Selection is view-only. Resolve identity at use time; never retain a node
// pointer across a graph edit. Source is not a selectable processing node.
Node *selectedNode(App &app);
const Node *selectedNode(const App &app);
bool selectNode(App &app, std::optional<NodeId> id);
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

// Capture preserves unavailable processors and opaque parameter values.
bool captureNode(const App &app, const NodeId &id, PersistNode &out);
// Narrow 6D guard: capture only an ordinary complete linear processor spine.
// Throws runtime_error before output is written when V2 would lose topology.
PersistChain captureChain(const App &app);

// Restoring persisted nodes uses the same structural transaction boundary.
bool appendPersistedNode(App &app, const PersistNode &node, std::optional<NodeId> insertAfter = std::nullopt);
void applyChain(App &app, const PersistChain &chain);

// Capture all stored nodes. Caller owns graph/processor execution exclusion;
// these worker-free helpers never schedule or publish a live document.
PersistGraph captureGraph(GraphView graph);

namespace document_detail {
struct RestoredGraph {
  std::vector<Node> nodes;
  NodeId primarySource;
  OutputRef output{NodeId{}, ports::image};
};
RestoredGraph restoreGraph(const PersistGraph &graph);

// Operation-local restoration, so grade application can prepare nodes and
// colour/source changes before replacing anything. Both helpers borrow their
// document operation's DocumentMutation; neither schedules preview work.
struct RestoredChain {
  std::vector<Node> nodes;
  OutputRef output{NodeId{}, ports::image};
  std::optional<NodeId> selection;
};
RestoredChain restoreChain(App &app, const PersistChain &chain);
void replaceChain(App &app, RestoredChain &&chain);
}  // namespace document_detail

bool hasUnknownProcessorChoiceIds(const App &app);
