#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

Node *selectedNode(App &app);
std::string nodeDisplayName(const Node &node);
void clearNodes(App &app);
bool addNode(App &app, int pluginIndex);
void destroyNode(App &app, int index);
void moveNode(App &app, int from, int to);

void syncOutputTag(App &app);
void applyColorDefaults(App &app, Node &node);

PersistChain captureChain(const App &app);
void applyChain(App &app, const PersistChain &chain);
