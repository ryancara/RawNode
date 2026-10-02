#pragma once

#include "processors/Processor.h"
#include "ofx/OfxHost.h"

#include <memory>

class OfxProcessor final : public Processor {
 public:
  static std::unique_ptr<OfxProcessor> create(int pluginIndex);
  ~OfxProcessor() override;

  ProcessorBackend backend() const override { return ProcessorBackend::OFX; }
  std::string identifier() const override;
  std::string displayName() const override;
  std::vector<ProcessorParameter> parameters() const override;
  bool setParameterValue(const std::string &id, const ParameterValue &value, bool notify = true) override;
  bool resetParameter(const std::string &id, bool notify = true) override;
  bool activateParameter(const std::string &id) override;
  void setRenderSize(int width, int height) override;
  ProcessorResult render(const Image &input, Image &output, int generation) override;

  int pluginIndex() const { return pluginIndex_; }
  Effect *effect() { return instance_.get(); }
  const Effect *effect() const { return instance_.get(); }

 private:
  OfxProcessor(int pluginIndex, std::unique_ptr<Effect> instance);
  void notifyChanged(Param *param);

  int pluginIndex_ = -1;
  std::unique_ptr<Effect> instance_;
};
