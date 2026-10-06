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
  ProcessorResult render(const Image &input, Image &output, const RenderCancellation &cancellation) override;

 private:
  // Store stable enum values, not menu positions.
  std::atomic<int> inputSpace_{(int)RgbGamut::Rec2020};
  std::atomic<int> inputGamma_{(int)TransferFunction::Linear};
  std::atomic<int> outputSpace_{(int)RgbGamut::Rec2020};
  std::atomic<int> outputGamma_{(int)TransferFunction::Linear};
};
