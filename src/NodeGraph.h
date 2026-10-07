#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

// The current graph is App::nodes in serial order. These operations own node
// identity, processor lifetime and conversion to/from persistence records.
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
