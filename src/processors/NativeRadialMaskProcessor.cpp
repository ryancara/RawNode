#include "processors/NativeRadialMaskProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct ParameterSpec {
  const char *id, *label, *hint;
  double initial, min, max;
};
constexpr ParameterSpec specs[] = {
    {"centreX", "Centre X", "Fraction of Reference width from the left edge", 0.5, -1, 2},
    {"centreY", "Centre Y", "Fraction of Reference height from the bottom edge", 0.5, -1, 2},
    {"radiusX", "Horizontal radius", "Outer radius as a fraction of Reference width", 0.25, 0, 2},
    {"radiusY", "Vertical radius", "Outer radius as a fraction of Reference height", 0.25, 0, 2},
    {"feather", "Feather", "Inward transition as a fraction of the outer radius (0 = hard edge)", 0.5, 0, 1},
};
}

NativeRadialMaskProcessor::NativeRadialMaskProcessor() {
  for (size_t i = 0; i < values_.size(); ++i) values_[i].store(specs[i].initial, std::memory_order_relaxed);
}

std::vector<ProcessorParameter> NativeRadialMaskProcessor::parameters() const {
  std::vector<ProcessorParameter> result;
  for (size_t i = 0; i < values_.size(); ++i) {
    const auto &spec = specs[i];
    ProcessorParameter param;
    param.id = spec.id; param.label = spec.label; param.hint = spec.hint;
    param.type = ParameterType::Double;
    param.value = values_[i].load(std::memory_order_relaxed);
    param.defaultValue = spec.initial;
    param.min = param.displayMin = spec.min;
    param.max = param.displayMax = spec.max;
    param.step = 0.01;
    result.push_back(std::move(param));
  }
  return result;
}

bool NativeRadialMaskProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool) {
  const auto *number = std::get_if<double>(&value);
  if (!number || !std::isfinite(*number)) return false;
  for (size_t i = 0; i < values_.size(); ++i) if (id == specs[i].id) {
    values_[i].store(std::clamp(*number, specs[i].min, specs[i].max), std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool NativeRadialMaskProcessor::resetParameter(const std::string &id, bool notify) {
  for (const auto &spec : specs) if (id == spec.id) return setParameterValue(id, spec.initial, notify);
  return false;
}

ProcessorResult NativeRadialMaskProcessor::render(const Image &, Image &, const RenderCancellation &) {
  return ProcessorResult::failure(-1, "Radial Mask produces Mask data, not an Image");
}

ProcessorResult NativeRadialMaskProcessor::generateMask(int width, int height, Mask &output,
                                                       const RenderCancellation &cancellation) const {
  if (width <= 0 || height <= 0 || size_t(width) > std::numeric_limits<size_t>::max() / size_t(height))
    return ProcessorResult::failure(-1, "Invalid Reference dimensions");
  const double cx = values_[0].load(std::memory_order_relaxed), cy = values_[1].load(std::memory_order_relaxed);
  const double rx = values_[2].load(std::memory_order_relaxed), ry = values_[3].load(std::memory_order_relaxed);
  const double feather = values_[4].load(std::memory_order_relaxed);
  output.w = width; output.h = height;
  output.px.assign(size_t(width) * size_t(height), 0.0f);
  if (rx == 0 || ry == 0) return ProcessorResult::success();
  // Pixel centres in the Reference raster's normalised, bottom-up frame.
  // Feather moves inward from the outer ellipse, never changing its extent.
  for (int y = 0; y < height; ++y) {
    if (cancellation.cancelled()) return ProcessorResult::success(); // Evaluator discards cancelled work.
    const double dy = ((double(y) + 0.5) / height - cy) / ry;
    for (int x = 0; x < width; ++x) {
      const double dx = ((double(x) + 0.5) / width - cx) / rx;
      const double distance = std::hypot(dx, dy);
      const double t = feather == 0 ? (distance <= 1 ? 1.0 : 0.0)
          : std::clamp((1.0 - distance) / feather, 0.0, 1.0);
      output.px[size_t(y) * size_t(width) + size_t(x)] = float(t * t * (3.0 - 2.0 * t));
    }
  }
  return ProcessorResult::success();
}
