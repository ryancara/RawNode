#pragma once

#include "GraphModel.h"

struct App;

namespace ui {
struct ProcessorIdentity;
struct CommandStep;
struct ScalarCommandOperand;

enum class ScalarEdit { Set, Adjust, Increase, Decrease, Reset };

// Processing rows from the Nodes List, including its off-spine section.
std::vector<const Node *> processorRows(const App &app);
ProcessorIdentity processorIdentity(const Node &node);
const Node *resolveProcessor(const App &app, const ProcessorIdentity &identity);
bool selectCommandNode(App &app, const CommandStep &step, std::string &error);

bool editableScalar(const ProcessorParameter &param, const std::vector<ProcessorParameter> &parameters);
std::optional<ProcessorParameter> scalarParameter(const Node &node, const std::string &id);
double automaticStep(const ProcessorParameter &param);
std::optional<ParameterValue> scalarResult(const ProcessorParameter &param, ScalarEdit edit,
                                           double amount, std::string &error);
bool editScalarParameter(App &app, const Node &node, const std::string &id, ScalarEdit edit,
                         double operand, std::string &error);
bool editCommandParameter(App &app, const ScalarCommandOperand &operand, ScalarEdit edit, std::string &error);
}  // namespace ui
