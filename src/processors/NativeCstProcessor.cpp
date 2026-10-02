#include "processors/NativeCstProcessor.h"

#include "color/LinearColorTransform.h"

#include <array>

namespace {

constexpr std::array<ColorSpace, 3> kSpaces = {
    ColorSpace::LinearRec709,
    ColorSpace::LinearRec2020,
    ColorSpace::ACES2065_1,
};

constexpr const char *kSpaceNames[] = {
    "Linear Rec.709",
    "Linear Rec.2020",
    "ACES2065-1 (AP0)",
};

bool validSpaceIndex(int value) {
  return value >= 0 && value < (int)kSpaces.size();
}

ProcessorParameter makeSpaceParameter(const char *id, const char *label, int value) {
  ProcessorParameter param;
  param.id = id;
  param.label = label;
  param.hint = "Explicit scene-linear RGB colour space";
  param.type = ParameterType::Choice;
  param.value = value;
  param.defaultValue = 1;  // Linear Rec.2020; input/output defaults form an identity transform.
  param.choices = {kSpaceNames[0], kSpaceNames[1], kSpaceNames[2]};
  return param;
}

}  // namespace

std::vector<ProcessorParameter> NativeCstProcessor::parameters() const {
  return {
      makeSpaceParameter("input_space", "Input Colour Space", inputSpace_.load(std::memory_order_relaxed)),
      makeSpaceParameter("output_space", "Output Colour Space", outputSpace_.load(std::memory_order_relaxed)),
  };
}

bool NativeCstProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)notify;
  const int *choice = std::get_if<int>(&value);
  if (!choice || !validSpaceIndex(*choice)) return false;

  if (id == "input_space") {
    inputSpace_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_space") {
    outputSpace_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool NativeCstProcessor::resetParameter(const std::string &id, bool notify) {
  (void)notify;
  if (id == "input_space") {
    inputSpace_.store(1, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_space") {
    outputSpace_.store(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool NativeCstProcessor::activateParameter(const std::string &id) {
  (void)id;
  return false;
}

void NativeCstProcessor::setRenderSize(int width, int height) {
  (void)width;
  (void)height;
}

ProcessorResult NativeCstProcessor::render(const Image &input, Image &output, int generation) {
  (void)generation;

  const int inputIndex = inputSpace_.load(std::memory_order_relaxed);
  const int outputIndex = outputSpace_.load(std::memory_order_relaxed);
  if (!validSpaceIndex(inputIndex) || !validSpaceIndex(outputIndex))
    return ProcessorResult::failure(1, "Invalid CST colour-space selection");

  double matrix[3][3] = {};
  if (!linearColorTransformMatrix(kSpaces[(size_t)inputIndex], kSpaces[(size_t)outputIndex], matrix))
    return ProcessorResult::failure(2, "Unsupported CST colour-space transform");

  output = input;
  for (size_t i = 0; i + 3 < output.px.size(); i += 4) {
    const float in[3] = {input.px[i + 0], input.px[i + 1], input.px[i + 2]};
    float out[3] = {};
    applyLinearColorMatrix(matrix, in, out);
    output.px[i + 0] = out[0];
    output.px[i + 1] = out[1];
    output.px[i + 2] = out[2];
    // Alpha is copied unchanged by output = input.
  }
  return ProcessorResult::success();
}
