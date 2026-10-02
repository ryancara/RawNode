#pragma once

#include "AppState.h"
#include "persist/ProjectPersist.h"

Node *selectedNode(App &app);
std::string nodeDisplayName(const Node &node);
void clearNodes(App &app);
bool addNode(App &app, int pluginIndex);  // OFX plugin
bool addNativeExposureNode(App &app);
bool addCtlNode(App &app, const std::string &path);
void destroyNode(App &app, int index);
void moveNode(App &app, int from, int to);

struct InputColorSyncResult {
  int updated = 0;
  int unsupported = 0;
};

void syncOutputTag(App &app);
void applyColorDefaults(App &app, Node &node);
InputColorSyncResult syncOfxInputColorSpace(App &app, ColorSpace space);

PersistChain captureChain(const App &app);
void applyChain(App &app, const PersistChain &chain);
