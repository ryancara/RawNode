#include "processors/CtlProcessor.h"

#include <CtlFunctionCall.h>
#include <CtlInterpreter.h>
#include <CtlSimdInterpreter.h>
#include <CtlStdType.h>
#include <CtlType.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool validVaryingFloat(const Ctl::FunctionArgPtr &arg) {
  return arg.refcount() != 0 && arg->isVarying() &&
         arg->type().cast<Ctl::FloatType>().refcount() != 0;
}

std::string canonicalScriptPath(const std::string &path) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(fs::path(path), ec);
  if (ec) return path;
  const fs::path canonical = fs::weakly_canonical(absolute, ec);
  return ec ? absolute.string() : canonical.string();
}

}  // namespace

struct CtlProcessor::Impl {
  Ctl::SimdInterpreter interpreter;
  Ctl::FunctionCallPtr function;
  Ctl::FunctionArgPtr rIn;
  Ctl::FunctionArgPtr gIn;
  Ctl::FunctionArgPtr bIn;
  Ctl::FunctionArgPtr aIn;
  Ctl::FunctionArgPtr rOut;
  Ctl::FunctionArgPtr gOut;
  Ctl::FunctionArgPtr bOut;
  Ctl::FunctionArgPtr aOut;
  std::mutex mutex;

  void load(const std::string &path) {
    std::vector<std::string> modulePaths = Ctl::Interpreter::modulePaths();
    const std::string parent = fs::path(path).parent_path().string();
    if (!parent.empty() && std::find(modulePaths.begin(), modulePaths.end(), parent) == modulePaths.end())
      modulePaths.insert(modulePaths.begin(), parent);
    interpreter.setUserModulePath(modulePaths, true);

    interpreter.loadFile(path);
    function = interpreter.newFunctionCall("main");

    if (function->returnValue()->type().cast<Ctl::VoidType>().refcount() == 0)
      throw std::runtime_error("CTL main() must return void");

    rIn = function->findInputArg("rIn");
    gIn = function->findInputArg("gIn");
    bIn = function->findInputArg("bIn");
    aIn = function->findInputArg("aIn");
    rOut = function->findOutputArg("rOut");
    gOut = function->findOutputArg("gOut");
    bOut = function->findOutputArg("bOut");
    aOut = function->findOutputArg("aOut");

    if (!validVaryingFloat(rIn) || !validVaryingFloat(gIn) || !validVaryingFloat(bIn))
      throw std::runtime_error("CTL main() must provide varying float rIn, gIn and bIn inputs");
    if (!validVaryingFloat(rOut) || !validVaryingFloat(gOut) || !validVaryingFloat(bOut))
      throw std::runtime_error("CTL main() must provide varying float rOut, gOut and bOut outputs");
    if (aIn.refcount() != 0 && !validVaryingFloat(aIn))
      throw std::runtime_error("CTL aIn must be a varying float when present");
    if (aOut.refcount() != 0 && !validVaryingFloat(aOut))
      throw std::runtime_error("CTL aOut must be a varying float when present");

    for (size_t i = 0; i < function->numInputArgs(); ++i) {
      Ctl::FunctionArgPtr arg = function->inputArg(i);
      const std::string &name = arg->name();
      if (name == "rIn" || name == "gIn" || name == "bIn" || name == "aIn") continue;
      if (!arg->hasDefaultValue())
        throw std::runtime_error("Unsupported required CTL input parameter: " + name);
      arg->setDefaultValue();
    }

    if (aIn.refcount() != 0 && aIn->hasDefaultValue()) aIn->setDefaultValue();
  }
};

CtlProcessor::CtlProcessor(std::string path, std::string name, std::unique_ptr<Impl> impl)
    : path_(std::move(path)), name_(std::move(name)), impl_(std::move(impl)) {}

CtlProcessor::~CtlProcessor() = default;

std::unique_ptr<CtlProcessor> CtlProcessor::create(const std::string &path, std::string *error) {
  const std::string canonical = canonicalScriptPath(path);
  std::error_code ec;
  if (!fs::is_regular_file(canonical, ec)) {
    if (error) *error = "CTL file not found";
    return nullptr;
  }

  try {
    auto impl = std::make_unique<Impl>();
    impl->load(canonical);
    std::string name = fs::path(canonical).stem().string();
    if (name.empty()) name = "CTL";
    return std::unique_ptr<CtlProcessor>(
        new CtlProcessor(canonical, std::move(name), std::move(impl)));
  } catch (const std::exception &e) {
    if (error) *error = e.what();
    return nullptr;
  } catch (...) {
    if (error) *error = "Unknown CTL interpreter error";
    return nullptr;
  }
}

std::string CtlProcessor::identifier() const { return path_; }

std::string CtlProcessor::displayName() const { return name_; }

bool CtlProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)id;
  (void)value;
  (void)notify;
  return false;
}

bool CtlProcessor::resetParameter(const std::string &id, bool notify) {
  (void)id;
  (void)notify;
  return false;
}

bool CtlProcessor::activateParameter(const std::string &id) {
  (void)id;
  return false;
}

void CtlProcessor::setRenderSize(int width, int height) {
  (void)width;
  (void)height;
}

ProcessorResult CtlProcessor::render(const Image &input, Image &output, int generation) {
  (void)generation;
  if (!impl_) return ProcessorResult::failure(-1, "Invalid CTL processor");

  std::lock_guard<std::mutex> lock(impl_->mutex);
  try {
    output.w = input.w;
    output.h = input.h;
    output.px.resize(input.px.size());

    const size_t pixels = (size_t)input.w * input.h;
    size_t offset = 0;
    while (offset < pixels) {
      const size_t count = std::min(impl_->interpreter.maxSamples(), pixels - offset);

      float *rIn = reinterpret_cast<float *>(impl_->rIn->data());
      float *gIn = reinterpret_cast<float *>(impl_->gIn->data());
      float *bIn = reinterpret_cast<float *>(impl_->bIn->data());
      float *aIn = impl_->aIn.refcount() ? reinterpret_cast<float *>(impl_->aIn->data()) : nullptr;

      for (size_t i = 0; i < count; ++i) {
        const size_t p = (offset + i) * 4;
        rIn[i] = input.px[p + 0];
        gIn[i] = input.px[p + 1];
        bIn[i] = input.px[p + 2];
        if (aIn) aIn[i] = input.px[p + 3];
      }

      impl_->function->callFunction(count);

      const float *rOut = reinterpret_cast<const float *>(impl_->rOut->data());
      const float *gOut = reinterpret_cast<const float *>(impl_->gOut->data());
      const float *bOut = reinterpret_cast<const float *>(impl_->bOut->data());
      const float *aOut = impl_->aOut.refcount()
                              ? reinterpret_cast<const float *>(impl_->aOut->data())
                              : nullptr;

      for (size_t i = 0; i < count; ++i) {
        const size_t p = (offset + i) * 4;
        output.px[p + 0] = rOut[i];
        output.px[p + 1] = gOut[i];
        output.px[p + 2] = bOut[i];
        output.px[p + 3] = aOut ? aOut[i] : input.px[p + 3];
      }

      offset += count;
    }

    return ProcessorResult::success();
  } catch (const std::exception &e) {
    return ProcessorResult::failure(-1, e.what());
  } catch (...) {
    return ProcessorResult::failure(-1, "Unknown CTL render error");
  }
}
