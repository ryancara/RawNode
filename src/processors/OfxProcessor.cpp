#include "processors/OfxProcessor.h"

#include <string>

OfxProcessor::OfxProcessor(int pluginIndex, std::unique_ptr<Effect> instance)
    : pluginIndex_(pluginIndex), instance_(std::move(instance)) {}

std::unique_ptr<OfxProcessor> OfxProcessor::create(int pluginIndex) {
  if (pluginIndex < 0 || pluginIndex >= (int)gPlugins.size()) return nullptr;
  auto instance = createInstance(gPlugins[pluginIndex]);
  if (!instance) return nullptr;
  return std::unique_ptr<OfxProcessor>(new OfxProcessor(pluginIndex, std::move(instance)));
}

OfxProcessor::~OfxProcessor() {
  if (!instance_ || pluginIndex_ < 0 || pluginIndex_ >= (int)gPlugins.size()) return;
  callAction(gPlugins[pluginIndex_].plugin, kOfxActionDestroyInstance, instance_.get());
}

std::string OfxProcessor::identifier() const {
  if (pluginIndex_ < 0 || pluginIndex_ >= (int)gPlugins.size()) return {};
  OfxPlugin *plugin = gPlugins[pluginIndex_].plugin;
  return plugin && plugin->pluginIdentifier ? plugin->pluginIdentifier : "";
}

std::string OfxProcessor::displayName() const {
  if (pluginIndex_ < 0 || pluginIndex_ >= (int)gPlugins.size()) return "OFX";
  return gPlugins[pluginIndex_].label;
}

void OfxProcessor::setRenderSize(int width, int height) {
  if (!instance_) return;
  instance_->w = width;
  instance_->h = height;
}

ProcessorResult OfxProcessor::render(const Image &input, Image &output, int generation) {
  if (!instance_ || pluginIndex_ < 0 || pluginIndex_ >= (int)gPlugins.size())
    return ProcessorResult::failure(-1, "Invalid OFX processor");

  OfxPlugin *plugin = gPlugins[pluginIndex_].plugin;
  int outW = input.w;
  int outH = input.h;
  queryOutputSize(plugin, instance_.get(), input.w, input.h, &outW, &outH);

  output.w = outW;
  output.h = outH;
  output.px.resize((size_t)outW * outH * 4);

  const OfxStatus status =
      renderEffect(plugin, instance_.get(), const_cast<float *>(input.px.data()), output.px.data(),
                   input.w, input.h, outW, outH, generation);
  if (status != kOfxStatOK)
    return ProcessorResult::failure((int)status, "OFX status " + std::to_string((int)status));

  return ProcessorResult::success();
}
