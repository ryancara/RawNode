#pragma once

#include "processors/Processor.h"

#include <memory>
#include <string>

class CtlProcessor final : public Processor {
 public:
  static std::unique_ptr<CtlProcessor> create(const std::string &path, std::string *error = nullptr);
  ~CtlProcessor() override;

  ProcessorBackend backend() const override { return ProcessorBackend::CTL; }
  std::string identifier() const override;
  std::string displayName() const override;

  std::vector<ProcessorParameter> parameters() const override;
  bool setParameterValue(const std::string &id, const ParameterValue &value, bool notify = true) override;
  bool resetParameter(const std::string &id, bool notify = true) override;
  bool activateParameter(const std::string &id) override;

  void setRenderSize(int width, int height) override;
  ProcessorResult render(const Image &input, Image &output, const RenderCancellation &cancellation) override;

 private:
  struct Impl;

  CtlProcessor(std::string path, std::string name, std::unique_ptr<Impl> impl);

  std::string path_;
  std::string name_;
  std::unique_ptr<Impl> impl_;
};
