#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

// App::nodes is the single graph storage, including Source. Execution remains
// serial temporarily; these lookups never interpret storage position as an ID.
Node *findNode(App &app, const NodeId &id);
const Node *findNode(const App &app, const NodeId &id);
const PortDeclaration *findOutput(const App &app, const OutputRef &output);
const std::optional<OutputRef> *findInput(const App &app, const InputAddress &input);
// Structural validation includes all stored edges/types/cycles. Disconnected
// required inputs remain representable; see requestedGraphError for readiness.
// Empty means valid; otherwise the first invariant violation is returned.
std::string documentStateError(const App &app);

// Legacy List View commands still take storage indices until Phase 5.
Node *selectedNode(App &app);
std::string nodeDisplayName(const Node &node);
// Structural operations run on the single control thread. Each owns execution
// safety and its final preview decision; callers need no renderer protocol.
void clearNodes(App &app);
bool addNode(App &app, int pluginIndex);  // OFX plugin
bool addNativeExposureNode(App &app);
bool addNativeCstNode(App &app);
bool addCtlNode(App &app, const std::string &path);
void destroyNode(App &app, int index);
void moveNode(App &app, int from, int to);
void setNodeEnabled(App &app, int index, bool enabled);

// Capture preserves unavailable processors and opaque parameter values.
bool captureNode(const App &app, int index, PersistNode &out);
PersistChain captureChain(const App &app);

// Restoring persisted nodes uses the same structural transaction boundary.
bool appendPersistedNode(App &app, const PersistNode &node, int insertAfter = -1);
void applyChain(App &app, const PersistChain &chain);

bool hasUnknownProcessorChoiceIds(const App &app);
