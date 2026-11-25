#pragma once

#include "GraphModel.h"

struct App;

namespace ui {
struct ProcessorIdentity;
struct CommandStep;

enum class ScalarEdit { Set, Adjust, Increase, Decrease, Reset };

// Processing rows from the Nodes List, including its off-spine section.
std::vector<const Node *> processorRows(const App &app);
ProcessorIdentity processorIdentity(const Node &node);
const Node *resolveProcessor(const App &app, const ProcessorIdentity &identity);
bool selectCommandNode(App &app, const CommandStep &step, std::string &error);

bool editableScalar(const ProcessorParameter &param, const std::vector<ProcessorParameter> &parameters);
bool targetParameter(App &app, const std::string &id, std::string &error);
bool editScalarParameter(App &app, const Node &node, const std::string &id, ScalarEdit edit,
                         double operand, std::string &error);
}  // namespace ui
