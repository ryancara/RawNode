#pragma once

#include "GraphModel.h"
#include "ui/KeyboardBindings.h"

struct App;

namespace ui {
// Processing rows from the Nodes List, including its off-spine section.
std::vector<const Node *> processorRows(const App &app);
ProcessorIdentity processorIdentity(const Node &node);
const Node *resolveProcessor(const App &app, const ProcessorIdentity &identity);
bool selectCommandNode(App &app, const CommandStep &step);

bool editableScalar(const ProcessorParameter &param, const std::vector<ProcessorParameter> &parameters);
bool targetParameter(App &app, const std::string &id);
bool editScalarParameter(App &app, const Node &node, const std::string &id, const CommandStep &step);
}  // namespace ui
