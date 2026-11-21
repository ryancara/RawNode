#include "processors/NativeCstProcessor.h"

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

  if (!transformImageEncoding(input, output, {(RgbGamut)inputSpace, (TransferFunction)inputGamma},
                              {(RgbGamut)outputSpace, (TransferFunction)outputGamma}))
    return ProcessorResult::failure(2, "Unsupported CST colour-space transform");
  return ProcessorResult::success();
}
