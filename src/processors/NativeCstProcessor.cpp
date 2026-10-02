#include "processors/NativeCstProcessor.h"

#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"

#include <cmath>

namespace {

bool validGamutValue(int value) {
  return rgbGamutIndex((RgbGamut)value) >= 0;
}

bool validGammaValue(int value) {
  return transferFunctionIndex((TransferFunction)value) >= 0;
}

ProcessorParameter makeGamutParameter(const char *id, const char *label, int value) {
  ProcessorParameter param;
  param.id = id;
  param.label = label;
  param.hint = "RGB primaries / gamut. Transfer function is selected separately.";
  param.type = ParameterType::Choice;
  param.value = value;
  param.defaultValue = (int)RgbGamut::Rec2020;

  for (int i = 0; i < rgbGamutCount(); ++i) {
    const auto &def = rgbGamutDefinition(i);
    param.choices.emplace_back(def.name);
    param.choiceIds.emplace_back(def.id);
    param.choiceValues.emplace_back((int)def.value);
  }
  return param;
}

ProcessorParameter makeGammaParameter(const char *id, const char *label, int value) {
  ProcessorParameter param;
  param.id = id;
  param.label = label;
  param.hint = "Transfer function / encoding. RGB primaries are selected separately.";
  param.type = ParameterType::Choice;
  param.value = value;
  param.defaultValue = (int)TransferFunction::Linear;

  for (int i = 0; i < transferFunctionCount(); ++i) {
    const auto &def = transferFunctionDefinition(i);
    param.choices.emplace_back(def.name);
    param.choiceIds.emplace_back(def.id);
    param.choiceValues.emplace_back((int)def.value);
  }
  return param;
}

}  // namespace

std::vector<ProcessorParameter> NativeCstProcessor::parameters() const {
  return {
      makeGamutParameter("input_space", "Input Colour Space", inputSpace_.load(std::memory_order_relaxed)),
      makeGammaParameter("input_gamma", "Input Gamma", inputGamma_.load(std::memory_order_relaxed)),
      makeGamutParameter("output_space", "Output Colour Space", outputSpace_.load(std::memory_order_relaxed)),
      makeGammaParameter("output_gamma", "Output Gamma", outputGamma_.load(std::memory_order_relaxed)),
  };
}

bool NativeCstProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)notify;
  const int *choice = std::get_if<int>(&value);
  if (!choice) return false;

  if (id == "input_space" && validGamutValue(*choice)) {
    inputSpace_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  if (id == "input_gamma" && validGammaValue(*choice)) {
    inputGamma_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_space" && validGamutValue(*choice)) {
    outputSpace_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_gamma" && validGammaValue(*choice)) {
    outputGamma_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool NativeCstProcessor::resetParameter(const std::string &id, bool notify) {
  (void)notify;
  if (id == "input_space") {
    inputSpace_.store((int)RgbGamut::Rec2020, std::memory_order_relaxed);
    return true;
  }
  if (id == "input_gamma") {
    inputGamma_.store((int)TransferFunction::Linear, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_space") {
    outputSpace_.store((int)RgbGamut::Rec2020, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_gamma") {
    outputGamma_.store((int)TransferFunction::Linear, std::memory_order_relaxed);
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

  const int inputSpace = inputSpace_.load(std::memory_order_relaxed);
  const int inputGamma = inputGamma_.load(std::memory_order_relaxed);
  const int outputSpace = outputSpace_.load(std::memory_order_relaxed);
  const int outputGamma = outputGamma_.load(std::memory_order_relaxed);
  if (!validGamutValue(inputSpace) || !validGamutValue(outputSpace) ||
      !validGammaValue(inputGamma) || !validGammaValue(outputGamma))
    return ProcessorResult::failure(1, "Invalid CST selection");

  double matrix[3][3] = {};
  if (!linearColorTransformMatrix((RgbGamut)inputSpace, (RgbGamut)outputSpace, matrix))
    return ProcessorResult::failure(2, "Unsupported CST colour-space transform");

  const TransferFunction inTf = (TransferFunction)inputGamma;
  const TransferFunction outTf = (TransferFunction)outputGamma;

  output = input;
  for (size_t i = 0; i + 3 < output.px.size(); i += 4) {
    const float ir = input.px[i + 0];
    const float ig = input.px[i + 1];
    const float ib = input.px[i + 2];

    // One bad upstream pixel must not invalidate the entire frame. Preserve it
    // unchanged so the issue remains local and visible to downstream tools.
    if (!std::isfinite(ir) || !std::isfinite(ig) || !std::isfinite(ib))
      continue;

    const double dr = decodeTransfer(ir, inTf);
    const double dg = decodeTransfer(ig, inTf);
    const double db = decodeTransfer(ib, inTf);
    if (!std::isfinite(dr) || !std::isfinite(dg) || !std::isfinite(db))
      continue;

    const float decoded[3] = {(float)dr, (float)dg, (float)db};
    float converted[3] = {};
    applyLinearColorMatrix(matrix, decoded, converted);
    if (!std::isfinite(converted[0]) || !std::isfinite(converted[1]) ||
        !std::isfinite(converted[2]))
      continue;

    const double r = encodeTransfer(converted[0], outTf);
    const double g = encodeTransfer(converted[1], outTf);
    const double b = encodeTransfer(converted[2], outTf);
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
      continue;

    output.px[i + 0] = (float)r;
    output.px[i + 1] = (float)g;
    output.px[i + 2] = (float)b;
    // Alpha is copied unchanged by output = input.
  }
  return ProcessorResult::success();
}
