#include "ui/CommandTargets.h"

#include "NodeGraph.h"
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

bool selectCommandNode(App &app, const CommandStep &step) {
  if (step.kind == Command::SelectProcessor) {
    const auto &identity = std::get<ProcessorIdentity>(step.operand);
    if (const auto *node = resolveProcessor(app, identity)) return selectNode(app, node->id);
    app.setStatus("Processor unavailable: " + identity.backend + " / " + identity.identifier);
    return false;
  }
  const auto rows = processorRows(app);
  const auto current = std::find(rows.begin(), rows.end(), selectedNode(app));
  int index = static_cast<int>(current - rows.begin());
  switch (step.kind) {
    case Command::SelectCurrent: break;
    case Command::SelectPosition: index = std::get<int>(step.operand) - 1; break;
    case Command::SelectNext: index = current == rows.end() ? 0 : index + 1; break;
    case Command::SelectPrevious: --index; break;
    default: return false;
  }
  if (index >= 0 && index < static_cast<int>(rows.size())) return selectNode(app, rows[index]->id);
  app.setStatus("No processing node at the requested List position");
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

bool targetParameter(App &app, const std::string &id) {
  const auto *node = selectedNode(app);
  if (!node || !scalarParameter(*node, id)) {
    app.setStatus("Editable scalar parameter unavailable: " + id);
    return false;
  }
  app.targetedParameter = id;
  return true;
}

static double automaticStep(const ProcessorParameter &param) {
  if (param.type == ParameterType::Integer) {
    return param.step > 0 && param.step <= std::numeric_limits<int>::max() &&
           std::trunc(param.step) == param.step ? param.step : 1;
  }
  if (std::isfinite(param.step) && param.step > 0) return param.step;
  if (!param.hasRange || !param.hasDisplayRange) return 0;
  const double step = (param.displayMax - param.displayMin) / 100;
  return std::isfinite(step) && step > 0 ? step : 0;
}

bool editScalarParameter(App &app, const Node &node, const std::string &id, const CommandStep &step) {
  if (!requireEditableGraph(app) || !app.renderer.canEditParameters()) return false;
  const auto param = scalarParameter(node, id);
  if (!param) {
    app.setStatus("Editable scalar parameter unavailable: " + id);
    return false;
  }

  if (step.kind == Command::ResetParameter) {
    if (param->value == param->defaultValue) return true;
    if (!resetNodeParameter(app, node.id, id)) return false;
  } else {
    const bool integer = param->type == ParameterType::Integer;
    const double current = integer ? std::get<int>(param->value) : std::get<double>(param->value);
    double value;
    if (step.kind == Command::Increase || step.kind == Command::Decrease) {
      const double amount = automaticStep(*param);
      if (amount == 0) {
        app.setStatus("No automatic step for " + id + "; enter an explicit amount");
        return false;
      }
      value = step.kind == Command::Increase ? current + amount : current - amount;
    } else {
      value = std::get<double>(step.operand);
      if (integer && std::trunc(value) != value) {
        app.setStatus("An integer parameter requires a whole number");
        return false;
      }
      if (step.kind == Command::AdjustValue) value += current;
    }
    if (!std::isfinite(value) || (integer && (value < std::numeric_limits<int>::min() ||
                                             value > std::numeric_limits<int>::max()))) {
      app.setStatus("Value outside the representable range for " + id);
      return false;
    }
    if (param->hasRange && param->min <= param->max) {
      // Intersect hard bounds with the type's domain before integer conversion.
      double low = param->min, high = param->max;
      if (integer) {
        low = std::max(std::ceil(low), double(std::numeric_limits<int>::min()));
        high = std::min(std::floor(high), double(std::numeric_limits<int>::max()));
      }
      if (low > high) { app.setStatus("Empty scalar range for " + id); return false; }
      value = std::clamp(value, low, high);
    }
    ParameterValue result = integer ? ParameterValue(static_cast<int>(value)) : ParameterValue(value);
    if (param->value == result) return true;
    if (!setNodeParameterValue(app, node.id, id, result)) return false;
  }
  return app.path.empty() || app.history.recordCompletedEdit(app);
}
}  // namespace ui
