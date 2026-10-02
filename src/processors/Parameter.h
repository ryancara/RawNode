#pragma once

#include <string>
#include <variant>
#include <vector>

enum class ParameterType {
  Double,
  Integer,
  Boolean,
  Choice,
  String,
  Custom,
  Vector,
  Group,
  Page,
  PushButton,
  Unsupported,
};

using ParameterValue =
    std::variant<std::monostate, double, int, bool, std::string, std::vector<double>>;

struct ProcessorParameter {
  std::string id;
  std::string label;
  std::string parent;
  std::string hint;
  ParameterType type = ParameterType::Unsupported;

  ParameterValue value;
  ParameterValue defaultValue;

  double min = 0.0;
  double max = 1.0;
  double displayMin = 0.0;
  double displayMax = 1.0;
  // Some backends (notably plain CTL) expose numeric values without any
  // declared min/max metadata. In that case the UI must not invent a slider
  // range and should use a direct numeric editor instead.
  bool hasRange = true;
  // Optional preferred increment for buttons/drags. A non-positive value lets
  // the UI derive a sensible step from the declared range.
  double step = 0.0;

  std::vector<std::string> choices;
  // Usually choices map to 0..N-1. Backends such as ART CTL may declare
  // explicit integer values for each choice; an empty vector keeps the normal
  // index mapping.
  std::vector<int> choiceValues;

  bool enabled = true;
  bool secret = false;
  bool readOnly = false;
  bool vectorIsInteger = false;
  bool groupInitiallyOpen = true;
};
