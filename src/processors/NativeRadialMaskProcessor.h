#pragma once

#include "Mask.h"
#include "processors/Processor.h"

#include <array>
#include <atomic>

// Uses Processor's parameter/persistence API, but only the concrete native
// graph operation calls generateMask. Image backends gain no Mask interface.
class NativeRadialMaskProcessor final : public Processor {
 public:
  static constexpr const char *kIdentifier = "rawnode.native.radial-mask";
  NativeRadialMaskProcessor();

  ProcessorBackend backend() const override { return ProcessorBackend::Native; }
  std::string identifier() const override { return kIdentifier; }
  std::string displayName() const override { return "Radial Mask"; }
  std::vector<ProcessorParameter> parameters() const override;
  bool setParameterValue(const std::string &id, const ParameterValue &value, bool notify = true) override;
  bool resetParameter(const std::string &id, bool notify = true) override;
  bool activateParameter(const std::string &) override { return false; }
  void setRenderSize(int, int) override {}  // Reference dimensions are supplied per generation.
  ProcessorResult render(const Image &, Image &, const RenderCancellation &) override;
  ProcessorResult generateMask(int width, int height, Mask &output, const RenderCancellation &cancellation) const;

 private:
  // Ordinary parameter edits can overlap an obsolete preview, as for Exposure.
  // Each generation reads the five scalars once; a new preview settles edits.
  std::array<std::atomic<double>, 5> values_;
};
