#include "ui/CommandTargets.h"

#include "NodeGraph.h"
#include "ui/KeyboardBindings.h"
#include "ui/NodeList.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ui {
using Command = ApplicationCommand;

std::vector<const Node *> processorRows(const App &app) {
  const auto list = projectNodeList({app.nodes, app.primarySource, app.documentOutput});
  std::vector<const Node *> processors;
  for (const auto *section : {&list.spine, &list.other}) {
    for (const auto &id : *section) {
      const Node *node = findNode(app, id);
      if (node && isProcessorOperation(node->operation)) processors.push_back(node);
    }
  }
  return processors;
}

ProcessorIdentity processorIdentity(const Node &node) {
  if (node.processor) return {processorBackendName(node.processor->backend()), node.processor->identifier()};
  return {node.storedBackend, node.storedIdentifier};
}

const Node *resolveProcessor(const App &app, const ProcessorIdentity &identity) {
  const auto matches = [&](const Node &node) {
    const auto actual = processorIdentity(node);
    return actual.backend == identity.backend && actual.identifier == identity.identifier;
  };
  const Node *selected = selectedNode(app);
  if (selected && isProcessorOperation(selected->operation) && matches(*selected)) return selected;
  for (const auto *node : processorRows(app))
    if (matches(*node)) return node;
  return nullptr;
}

bool selectCommandNode(App &app, const CommandStep &step, std::string &error) {
  if (step.kind == Command::SelectProcessor) {
    const auto &identity = std::get<ProcessorIdentity>(step.operand);
    if (const auto *node = resolveProcessor(app, identity)) return selectNode(app, node->id);
    error = "Processor unavailable: " + identity.backend + " / " + identity.identifier;
    return false;
  }
  const auto rows = processorRows(app);
  const auto current = std::find(rows.begin(), rows.end(), selectedNode(app));
  int index = static_cast<int>(current - rows.begin());
  switch (step.kind) {
    case Command::SelectPosition: index = std::get<int>(step.operand) - 1; break;
    case Command::SelectNext: index = current == rows.end() ? 0 : index + 1; break;
    case Command::SelectPrevious: --index; break;
    default: return false;
  }
  if (index >= 0 && index < static_cast<int>(rows.size())) return selectNode(app, rows[index]->id);
  if (step.kind == Command::SelectNext) error = "No next processing node";
  else if (step.kind == Command::SelectPrevious) error = "No previous processing node";
  else error = "No processing node at the requested List position";
  return false;
}

bool editableScalar(const ProcessorParameter &param, const std::vector<ProcessorParameter> &parameters) {
  if ((param.type != ParameterType::Double && param.type != ParameterType::Integer) ||
      param.secret || !param.enabled || param.readOnly) return false;
  // A hidden/missing parent hides its controls in the Inspector as well.
  const auto *child = &param;
  for (size_t depth = 0; !child->parent.empty(); ++depth) {
    if (depth >= parameters.size()) return false;
    auto parent = std::find_if(parameters.begin(), parameters.end(), [&](const auto &p) {
      return p.id == child->parent;
    });
    if (parent == parameters.end() || parent->secret || parent->type != ParameterType::Group) return false;
    child = &*parent;
  }
  return true;
}

static std::optional<ProcessorParameter> scalarParameter(const Node &node, const std::string &id) {
  if (!node.processor) return std::nullopt;
  const auto params = node.processor->parameters();
  for (const auto &param : params)
    if (param.id == id && editableScalar(param, params)) return param;
  return std::nullopt;
}

static double automaticStep(const ProcessorParameter &param) {
  if (param.type == ParameterType::Integer) {
    return param.step > 0 && param.step <= std::numeric_limits<int>::max() &&
           std::trunc(param.step) == param.step ? param.step : 1;
  }
  if (std::isfinite(param.step) && param.step > 0) return param.step;
  if (!param.hasRange) return 0;
  const double step = (param.displayMax - param.displayMin) / 100;
  return std::isfinite(step) && step > 0 ? step : 0;
}

static std::optional<ParameterValue> scalarResult(const ProcessorParameter &param, ScalarEdit edit,
                                                   double amount, std::string &error) {
  const bool integer = param.type == ParameterType::Integer;
  if (edit == ScalarEdit::Increase || edit == ScalarEdit::Decrease) {
    amount = automaticStep(param);
    if (amount == 0) {
      error = "No automatic step for " + param.id + ". Enter an explicit amount.";
      return std::nullopt;
    }
    if (edit == ScalarEdit::Decrease) amount = -amount;
  } else if (integer && std::trunc(amount) != amount) {
    error = "An integer parameter requires a whole number";
    return std::nullopt;
  }

  double value = amount;
  if (edit != ScalarEdit::Set)
    value += integer ? std::get<int>(param.value) : std::get<double>(param.value);
  if (!std::isfinite(value) || (integer && (value < std::numeric_limits<int>::min() ||
                                           value > std::numeric_limits<int>::max()))) {
    error = "Value outside the representable range for " + param.id;
    return std::nullopt;
  }
  if (param.hasRange && param.min <= param.max) {
    double low = param.min, high = param.max;
    if (integer) {
      low = std::max(std::ceil(low), double(std::numeric_limits<int>::min()));
      high = std::min(std::floor(high), double(std::numeric_limits<int>::max()));
    }
    if (low > high) {
      error = "Empty scalar range for " + param.id;
      return std::nullopt;
    }
    value = std::clamp(value, low, high);
  }
  return integer ? ParameterValue(static_cast<int>(value)) : ParameterValue(value);
}

bool editScalarParameter(App &app, const Node &node, const std::string &id, ScalarEdit edit,
                         double operand, std::string &error) {
  if (!requireEditableGraph(app) || !app.renderer.canEditParameters()) return false;
  const auto param = scalarParameter(node, id);
  if (!param) {
    error = "Editable scalar parameter unavailable: " + id;
    return false;
  }

  if (edit == ScalarEdit::Reset) {
    if (param->value == param->defaultValue) return true;
    if (!resetNodeParameter(app, node.id, id)) return false;
  } else {
    const auto value = scalarResult(*param, edit, operand, error);
    if (!value) return false;
    if (param->value == *value) return true;
    if (!setNodeParameterValue(app, node.id, id, *value)) return false;
  }
  return app.path.empty() || app.history.recordCompletedEdit(app);
}

bool editCommandParameter(App &app, const ScalarCommandOperand &operand, ScalarEdit edit, std::string &error) {
  const auto &identity = operand.address.processor;
  const auto *node = resolveProcessor(app, identity);
  if (!node) {
    error = "Processor unavailable: " + identity.backend + " / " + identity.identifier;
    return false;
  }
  return editScalarParameter(app, *node, operand.address.parameterId, edit, operand.value.value_or(0.0), error);
}

void adjustHeldParameter(App &app, const ScalarCommandOperand &operand, int arrowSteps, double wheelSteps) {
  auto &held = *app.previewHeldAdjustment;
  const auto &address = operand.address;
  if (held.target && held.target->value.empty()) return;
  const auto *node = held.target ? findNode(app, *held.target) : nullptr;
  if (!held.target) {
    if (arrowSteps == 0 && wheelSteps == 0) return;
    node = resolveProcessor(app, address.processor);
    held.target = node ? node->id : NodeId{};
    app.shortcutFailure.clear();
  }

  const auto param = node ? scalarParameter(*node, address.parameterId) : std::nullopt;
  const double step = operand.value ? *operand.value : param ? automaticStep(*param) : 0;
  const bool integer = param && param->type == ParameterType::Integer;
  std::string error;
  if (!node) error = "Processor unavailable: " + address.processor.identifier;
  else if (!param) error = "Editable scalar parameter unavailable: " + address.parameterId;
  else if (step <= 0) error = "No automatic step for " + param->id + ". Enter an explicit step.";
  else if (integer && std::trunc(step) != step) error = "An integer parameter requires a whole step.";
  if (!error.empty()) {
    held.target = NodeId{};
    // Keep owning the controls until release, even though this target is gone.
    if (app.parameterGesture && app.parameterGesture->heldAdjustment) finishParameterGesture(app, true);
    app.shortcutFailure = std::move(error);
    return;
  }

  if (integer) {
    const double accumulated = held.integerWheelRemainder + wheelSteps;
    wheelSteps = std::trunc(accumulated);
    held.integerWheelRemainder = accumulated - wheelSteps;
  }
  const double steps = arrowSteps + wheelSteps;
  if (steps == 0) return;
  const auto value = scalarResult(*param, ScalarEdit::Adjust, steps * step, error);
  if (!value) {
    app.shortcutFailure = std::move(error);
    return;
  }
  if (*value == param->value) return;
  if (!setNodeParameterValue(app, node->id, param->id, *value)) {
    app.shortcutFailure = "Could not adjust " + param->id;
    return;
  }
  if (!app.parameterGesture)
    app.parameterGesture = ParameterGesture{0, app.path, node->id, param->id, std::nullopt, false, true};
}
}  // namespace ui
