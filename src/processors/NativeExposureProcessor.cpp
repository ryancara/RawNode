#include "processors/NativeExposureProcessor.h"

#include <algorithm>
#include <cmath>

std::vector<ProcessorParameter> NativeExposureProcessor::parameters() const {
  ProcessorParameter exposure;
  exposure.id = "exposure";
  exposure.label = "Exposure";
  exposure.hint = "Scene-linear exposure in stops (EV)";
  exposure.type = ParameterType::Double;
  exposure.value = exposureEv_.load(std::memory_order_relaxed);
  exposure.defaultValue = 0.0;
  exposure.min = -10.0;
  exposure.max = 10.0;
  exposure.displayMin = -5.0;
  exposure.displayMax = 5.0;
  return {std::move(exposure)};
}

bool NativeExposureProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)notify;
  if (id != "exposure") return false;
  const double *ev = std::get_if<double>(&value);
  if (!ev || !std::isfinite(*ev)) return false;
  exposureEv_.store(std::clamp(*ev, -10.0, 10.0), std::memory_order_relaxed);
  return true;
}

bool NativeExposureProcessor::resetParameter(const std::string &id, bool notify) {
  (void)notify;
  if (id != "exposure") return false;
  exposureEv_.store(0.0, std::memory_order_relaxed);
  return true;
}

bool NativeExposureProcessor::activateParameter(const std::string &id) {
  (void)id;
  return false;
}

void NativeExposureProcessor::setRenderSize(int width, int height) {
  (void)width;
  (void)height;
}

ProcessorResult NativeExposureProcessor::render(const Image &input, Image &output, const RenderCancellation &cancellation) {
  (void)cancellation;

  output = input;
  const float gain = (float)std::exp2(exposureEv_.load(std::memory_order_relaxed));
  for (size_t i = 0; i + 3 < output.px.size(); i += 4) {
    output.px[i + 0] *= gain;
    output.px[i + 1] *= gain;
    output.px[i + 2] *= gain;
  }
  return ProcessorResult::success();
}
