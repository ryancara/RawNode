#pragma once

#include "processors/Processor.h"

class NativeExposureProcessor final : public Processor {
 public:
  static constexpr const char *kIdentifier = "rawnode.native.exposure";

  ProcessorBackend backend() const override { return ProcessorBackend::Native; }
  std::string identifier() const override { return kIdentifier; }
  std::string displayName() const override { return "Exposure"; }

  std::vector<ProcessorParameter> parameters() const override;
  bool setParameterValue(const std::string &id, const ParameterValue &value, bool notify = true) override;
  bool resetParameter(const std::string &id, bool notify = true) override;
  bool activateParameter(const std::string &id) override;

  void setRenderSize(int width, int height) override;
  ProcessorResult render(const Image &input, Image &output, int generation) override;

 private:
  double exposureEv_ = 0.0;
};
