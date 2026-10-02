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

  std::vector<std::string> choices;

  bool enabled = true;
  bool secret = false;
  bool readOnly = false;
  bool vectorIsInteger = false;
  bool groupInitiallyOpen = true;
};
