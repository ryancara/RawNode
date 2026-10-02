#include "processors/NativeCstProcessor.h"

#include "color/LinearColorTransform.h"
#include "color/TransferFunction.h"

#include <array>
#include <cmath>

namespace {

constexpr std::array<RgbGamut, 5> kGamuts = {
    RgbGamut::Rec709,
    RgbGamut::Rec2020,
    RgbGamut::ACES_AP0,
    RgbGamut::ACES_AP1,
    RgbGamut::DaVinciWideGamut,
};

constexpr std::array<TransferFunction, 4> kGammas = {
    TransferFunction::Linear,
    TransferFunction::SRGB,
    TransferFunction::Rec709,
    TransferFunction::DaVinciIntermediate,
};

bool validGamutIndex(int value) {
  return value >= 0 && value < (int)kGamuts.size();
}

bool validGammaIndex(int value) {
  return value >= 0 && value < (int)kGammas.size();
}

ProcessorParameter makeGamutParameter(const char *id, const char *label, int value) {
  ProcessorParameter param;
  param.id = id;
  param.label = label;
  param.hint = "RGB primaries / gamut. Transfer function is selected separately.";
  param.type = ParameterType::Choice;
  param.value = value;
  param.defaultValue = 1;  // Rec.2020
  for (RgbGamut gamut : kGamuts) param.choices.emplace_back(rgbGamutName(gamut));
  return param;
}

ProcessorParameter makeGammaParameter(const char *id, const char *label, int value) {
  ProcessorParameter param;
  param.id = id;
  param.label = label;
  param.hint = "Transfer function / encoding. RGB primaries are selected separately.";
  param.type = ParameterType::Choice;
  param.value = value;
  param.defaultValue = 0;  // Linear
  for (TransferFunction gamma : kGammas) param.choices.emplace_back(transferFunctionName(gamma));
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

  if (id == "input_space" && validGamutIndex(*choice)) {
    inputSpace_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  if (id == "input_gamma" && validGammaIndex(*choice)) {
    inputGamma_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_space" && validGamutIndex(*choice)) {
    outputSpace_.store(*choice, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_gamma" && validGammaIndex(*choice)) {
    outputGamma_.store(*choice, std::memory_order_relaxed);
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
  if (id == "input_gamma") {
    inputGamma_.store(0, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_space") {
    outputSpace_.store(1, std::memory_order_relaxed);
    return true;
  }
  if (id == "output_gamma") {
    outputGamma_.store(0, std::memory_order_relaxed);
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
  if (!validGamutIndex(inputSpace) || !validGamutIndex(outputSpace) ||
      !validGammaIndex(inputGamma) || !validGammaIndex(outputGamma))
    return ProcessorResult::failure(1, "Invalid CST selection");

  double matrix[3][3] = {};
  if (!linearColorTransformMatrix(kGamuts[(size_t)inputSpace], kGamuts[(size_t)outputSpace], matrix))
    return ProcessorResult::failure(2, "Unsupported CST colour-space transform");

  const TransferFunction inTf = kGammas[(size_t)inputGamma];
  const TransferFunction outTf = kGammas[(size_t)outputGamma];

  output = input;
  for (size_t i = 0; i + 3 < output.px.size(); i += 4) {
    const float decoded[3] = {
        (float)decodeTransfer(input.px[i + 0], inTf),
        (float)decodeTransfer(input.px[i + 1], inTf),
        (float)decodeTransfer(input.px[i + 2], inTf),
    };

    float converted[3] = {};
    applyLinearColorMatrix(matrix, decoded, converted);

    const double r = encodeTransfer(converted[0], outTf);
    const double g = encodeTransfer(converted[1], outTf);
    const double b = encodeTransfer(converted[2], outTf);
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
      return ProcessorResult::failure(3, "CST produced a non-finite value");

    output.px[i + 0] = (float)r;
    output.px[i + 1] = (float)g;
    output.px[i + 2] = (float)b;
    // Alpha is copied unchanged by output = input.
  }
  return ProcessorResult::success();
}
