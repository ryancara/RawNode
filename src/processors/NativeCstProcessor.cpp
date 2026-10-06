#include "processors/NativeCstProcessor.h"

#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"

#include <cmath>
#include <limits>

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

ProcessorResult NativeCstProcessor::render(const Image &input, Image &output, const RenderCancellation &cancellation) {
  (void)cancellation;

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
  const double floatMax = (double)std::numeric_limits<float>::max();
  const auto invalidatePixel = [&](size_t i) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    output.px[i + 0] = nan;
    output.px[i + 1] = nan;
    output.px[i + 2] = nan;
  };
  const auto finiteFloat = [&](double v) {
    if (v > floatMax) return std::numeric_limits<float>::max();
    if (v < -floatMax) return -std::numeric_limits<float>::max();
    return (float)v;
  };

  for (size_t i = 0; i + 3 < output.px.size(); i += 4) {
    const double ir = input.px[i + 0];
    const double ig = input.px[i + 1];
    const double ib = input.px[i + 2];

    // Never leave one pixel partly in the input encoding and partly in the
    // output encoding. Non-finite upstream RGB makes the whole RGB triplet
    // invalid while alpha remains untouched.
    if (!std::isfinite(ir) || !std::isfinite(ig) || !std::isfinite(ib)) {
      invalidatePixel(i);
      continue;
    }

    const double decoded[3] = {
        decodeTransfer(ir, inTf),
        decodeTransfer(ig, inTf),
        decodeTransfer(ib, inTf),
    };
    if (!std::isfinite(decoded[0]) || !std::isfinite(decoded[1]) ||
        !std::isfinite(decoded[2])) {
      invalidatePixel(i);
      continue;
    }

    double converted[3] = {};
    for (int row = 0; row < 3; ++row)
      converted[row] = matrix[row][0] * decoded[0] +
                       matrix[row][1] * decoded[1] +
                       matrix[row][2] * decoded[2];

    if (!std::isfinite(converted[0]) || !std::isfinite(converted[1]) ||
        !std::isfinite(converted[2])) {
      invalidatePixel(i);
      continue;
    }

    const double encoded[3] = {
        encodeTransfer(converted[0], outTf),
        encodeTransfer(converted[1], outTf),
        encodeTransfer(converted[2], outTf),
    };
    if (!std::isfinite(encoded[0]) || !std::isfinite(encoded[1]) ||
        !std::isfinite(encoded[2])) {
      invalidatePixel(i);
      continue;
    }

    output.px[i + 0] = finiteFloat(encoded[0]);
    output.px[i + 1] = finiteFloat(encoded[1]);
    output.px[i + 2] = finiteFloat(encoded[2]);
    // Alpha is copied unchanged by output = input.
  }
  return ProcessorResult::success();
}
