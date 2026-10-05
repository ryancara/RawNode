#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

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

bool captureNode(const App &app, int index, PersistNode &out);
bool appendPersistedNode(App &app, const PersistNode &node, int insertAfter = -1);
PersistChain captureChain(const App &app);
void applyChain(App &app, const PersistChain &chain);
bool hasUnknownProcessorChoiceIds(const App &app);
