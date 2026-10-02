#pragma once

#include "processors/Processor.h"

#include <atomic>

class NativeCstProcessor final : public Processor {
 public:
  static constexpr const char *kIdentifier = "rawnode.native.cst";

  ProcessorBackend backend() const override { return ProcessorBackend::Native; }
  std::string identifier() const override { return kIdentifier; }
  std::string displayName() const override { return "CST"; }

  std::vector<ProcessorParameter> parameters() const override;
  bool setParameterValue(const std::string &id, const ParameterValue &value, bool notify = true) override;
  bool resetParameter(const std::string &id, bool notify = true) override;
  bool activateParameter(const std::string &id) override;

  void setRenderSize(int width, int height) override;
  ProcessorResult render(const Image &input, Image &output, int generation) override;

 private:
  // Choice indices map to the scene-linear spaces exposed by the first CST
  // implementation: Rec.709, Rec.2020, ACES2065-1/AP0.
  std::atomic<int> inputSpace_{1};
  std::atomic<int> outputSpace_{1};
};
