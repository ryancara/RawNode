#include "processors/OfxProcessor.h"

#include "ofxParam.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

ParameterType parameterType(const Param &param) {
  const std::string &type = param.type;
  if (type == kOfxParamTypeDouble) return ParameterType::Double;
  if (type == kOfxParamTypeInteger) return ParameterType::Integer;
  if (type == kOfxParamTypeBoolean) return ParameterType::Boolean;
  if (type == kOfxParamTypeChoice) return ParameterType::Choice;
  if (type == kOfxParamTypeString) return ParameterType::String;
  if (type == kOfxParamTypeCustom) return ParameterType::Custom;
  if (type == kOfxParamTypeGroup) return ParameterType::Group;
  if (type == kOfxParamTypePage) return ParameterType::Page;
  if (type == kOfxParamTypePushButton) return ParameterType::PushButton;
  if (dims(type) > 1) return ParameterType::Vector;
  return ParameterType::Unsupported;
}

std::vector<std::string> choiceOptions(const Param &param) {
  std::vector<std::string> out;
  auto it = param.props.m.find(kOfxParamPropChoiceOption);
  if (it == param.props.m.end()) return out;
  out.reserve(it->second.size());
  for (const Val &v : it->second) out.push_back(v.s);
  return out;
}

ProcessorParameter snapshotParameter(const Param &param) {
  ProcessorParameter out;
  out.id = param.name;
  out.label = sprop(param.props, kOfxPropLabel);
  out.parent = sprop(param.props, kOfxParamPropParent);
  out.hint = sprop(param.props, kOfxParamPropHint);
  out.type = parameterType(param);
  out.enabled = dprop(param.props, kOfxParamPropEnabled, 0, 1) != 0;
  out.secret = dprop(param.props, kOfxParamPropSecret, 0, 0) != 0;
  out.groupInitiallyOpen = dprop(param.props, kOfxParamPropGroupOpen, 0, 1) != 0;

  switch (out.type) {
    case ParameterType::Double: {
      double lo = dprop(param.props, kOfxParamPropDisplayMin, 0, dprop(param.props, kOfxParamPropMin, 0, 0));
      double hi = dprop(param.props, kOfxParamPropDisplayMax, 0, dprop(param.props, kOfxParamPropMax, 0, 1));
      if (!(std::fabs(lo) < 1e7)) lo = 0;
      if (!(std::fabs(hi) < 1e7) || hi <= lo) hi = lo + 1;
      out.min = dprop(param.props, kOfxParamPropMin, 0, lo);
      out.max = dprop(param.props, kOfxParamPropMax, 0, hi);
      out.displayMin = lo;
      out.displayMax = hi;
      out.value = param.v.empty() ? 0.0 : param.v[0];
      out.defaultValue = dprop(param.props, kOfxParamPropDefault, 0, 0);
      break;
    }
    case ParameterType::Integer: {
      double lo = dprop(param.props, kOfxParamPropDisplayMin, 0, dprop(param.props, kOfxParamPropMin, 0, 0));
      double hi = dprop(param.props, kOfxParamPropDisplayMax, 0, dprop(param.props, kOfxParamPropMax, 0, 100));
      if (!(std::fabs(lo) < 1e7)) lo = 0;
      if (!(std::fabs(hi) < 1e7) || hi <= lo) hi = lo + 100;
      out.min = dprop(param.props, kOfxParamPropMin, 0, lo);
      out.max = dprop(param.props, kOfxParamPropMax, 0, hi);
      out.displayMin = lo;
      out.displayMax = hi;
      out.value = param.v.empty() ? 0 : (int)std::lround(param.v[0]);
      out.defaultValue = (int)std::lround(dprop(param.props, kOfxParamPropDefault, 0, 0));
      break;
    }
    case ParameterType::Boolean:
      out.value = !param.v.empty() && param.v[0] != 0;
      out.defaultValue = dprop(param.props, kOfxParamPropDefault, 0, 0) != 0;
      break;
    case ParameterType::Choice:
      out.value = param.v.empty() ? 0 : (int)std::lround(param.v[0]);
      out.defaultValue = (int)std::lround(dprop(param.props, kOfxParamPropDefault, 0, 0));
      out.choices = choiceOptions(param);
      break;
    case ParameterType::String:
    case ParameterType::Custom:
      out.value = param.s;
      out.defaultValue = sprop(param.props, kOfxParamPropDefault);
      out.readOnly = out.type == ParameterType::String &&
                     sprop(param.props, kOfxParamPropStringMode) == kOfxParamStringIsLabel;
      break;
    case ParameterType::Vector: {
      out.value = param.v;
      std::vector<double> defaults(param.v.size());
      for (size_t i = 0; i < defaults.size(); ++i)
        defaults[i] = dprop(param.props, kOfxParamPropDefault, (int)i, 0);
      out.defaultValue = std::move(defaults);
      out.vectorIsInteger = isIntType(param.type);
      break;
    }
    default:
      break;
  }
  return out;
}

}  // namespace

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

std::vector<ProcessorParameter> OfxProcessor::parameters() const {
  std::vector<ProcessorParameter> out;
  if (!instance_) return out;
  std::lock_guard<std::mutex> lock(gValueMutex);
  out.reserve(instance_->params.size());
  for (const auto &param : instance_->params) out.push_back(snapshotParameter(*param));
  return out;
}

void OfxProcessor::notifyChanged(Param *param) {
  if (!param || !instance_ || pluginIndex_ < 0 || pluginIndex_ >= (int)gPlugins.size()) return;

  PropSet in;
  OfxPropertySetHandle args = H(&in);
  const double scale[2] = {1, 1};
  propSetString(args, kOfxPropType, 0, kOfxTypeParameter);
  propSetString(args, kOfxPropName, 0, param->name.c_str());
  propSetString(args, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
  propSetDouble(args, kOfxPropTime, 0, 0);
  propSetN<double, propSetDouble>(args, kOfxImageEffectPropRenderScale, 2, scale);

  OfxPlugin *plugin = gPlugins[pluginIndex_].plugin;
  callAction(plugin, kOfxActionBeginInstanceChanged, instance_.get(), &in);
  callAction(plugin, kOfxActionInstanceChanged, instance_.get(), &in);
  callAction(plugin, kOfxActionEndInstanceChanged, instance_.get(), &in);
}

bool OfxProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  if (!instance_) return false;
  Param *param = findParam(instance_.get(), id.c_str());
  if (!param) return false;

  const ParameterType type = parameterType(*param);
  {
    std::lock_guard<std::mutex> lock(gValueMutex);
    switch (type) {
      case ParameterType::Double: {
        const double *v = std::get_if<double>(&value);
        if (!v || param->v.empty()) return false;
        param->v[0] = *v;
        break;
      }
      case ParameterType::Integer:
      case ParameterType::Choice: {
        const int *v = std::get_if<int>(&value);
        if (!v || param->v.empty()) return false;
        param->v[0] = *v;
        break;
      }
      case ParameterType::Boolean: {
        const bool *v = std::get_if<bool>(&value);
        if (!v || param->v.empty()) return false;
        param->v[0] = *v ? 1.0 : 0.0;
        break;
      }
      case ParameterType::String:
      case ParameterType::Custom: {
        const std::string *v = std::get_if<std::string>(&value);
        if (!v) return false;
        param->s = *v;
        break;
      }
      case ParameterType::Vector: {
        const auto *v = std::get_if<std::vector<double>>(&value);
        if (!v || v->size() != param->v.size()) return false;
        param->v = *v;
        break;
      }
      default:
        return false;
    }
  }

  if (notify) notifyChanged(param);
  return true;
}

bool OfxProcessor::resetParameter(const std::string &id, bool notify) {
  if (!instance_) return false;
  Param *param = findParam(instance_.get(), id.c_str());
  if (!param) return false;

  const ParameterType type = parameterType(*param);
  {
    std::lock_guard<std::mutex> lock(gValueMutex);
    if (type == ParameterType::String || type == ParameterType::Custom) {
      param->s = sprop(param->props, kOfxParamPropDefault);
    } else if (type == ParameterType::Double || type == ParameterType::Integer ||
               type == ParameterType::Boolean || type == ParameterType::Choice ||
               type == ParameterType::Vector) {
      for (size_t i = 0; i < param->v.size(); ++i)
        param->v[i] = dprop(param->props, kOfxParamPropDefault, (int)i, 0);
    } else {
      return false;
    }
  }

  if (notify) notifyChanged(param);
  return true;
}

bool OfxProcessor::activateParameter(const std::string &id) {
  if (!instance_) return false;
  Param *param = findParam(instance_.get(), id.c_str());
  if (!param || parameterType(*param) != ParameterType::PushButton) return false;
  notifyChanged(param);
  return true;
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
