#include "processors/CtlProcessor.h"

#include <CtlFunctionCall.h>
#include <CtlInterpreter.h>
#include <CtlMessage.h>
#include <CtlSimdInterpreter.h>
#include <CtlStdType.h>
#include <CtlType.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace {

// RawNode's own entry-point checks; their messages are already specific.
struct ContractError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// CTL reports compile and import errors through a process-wide message
// function (stderr by default) and then throws a generic exception such as
// 'Failed to load CTL module "module.1a2b3c4d"'. One router is installed once,
// before any interpreter exists, so CTL's unsynchronised global pointer is never
// swapped while a render thread may be calling CTL print(). The router forwards
// everything to the previous function and also copies messages raised on a
// thread that is currently loading a script.
thread_local std::string *tCapturedMessages = nullptr;
Ctl::MessageOutputFunction gPreviousMessageOutput = nullptr;

void routeCtlMessage(const std::string &message) {
  if (tCapturedMessages) *tCapturedMessages += message;
  if (gPreviousMessageOutput) gPreviousMessageOutput(message);
}

class ScopedCtlMessageCapture {
 public:
  ScopedCtlMessageCapture() {
    static std::once_flag installed;
    std::call_once(installed, [] { gPreviousMessageOutput = Ctl::setMessageOutputFunction(routeCtlMessage); });
    previous_ = tCapturedMessages;
    tCapturedMessages = &text_;
  }
  ~ScopedCtlMessageCapture() { tCapturedMessages = previous_; }
  ScopedCtlMessageCapture(const ScopedCtlMessageCapture &) = delete;
  ScopedCtlMessageCapture &operator=(const ScopedCtlMessageCapture &) = delete;

  const std::string &text() const { return text_; }

 private:
  std::string text_;
  std::string *previous_ = nullptr;
};

std::string trimmed(const std::string &text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

bool isCaretLine(const std::string &line) {
  return line.find('^') != std::string::npos && line.find_first_not_of(" \t^") == std::string::npos;
}

// Reduces captured CTL output to its diagnostics ("Script.ctl:3: Syntax Error."),
// dropping source echoes, caret markers and "(@errorN)" codes, and showing paths
// in the script's directory by file name.
std::string summarizeCtlMessages(const std::string &text, const std::string &scriptDir) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) lines.push_back(line);

  std::vector<std::string> diagnostics;
  for (size_t i = 0; i < lines.size(); ++i) {
    // CTL echoes the offending source line followed by a "^" marker line.
    if (i + 1 < lines.size() && isCaretLine(lines[i + 1])) {
      ++i;
      continue;
    }
    std::string line = lines[i];
    const size_t code = line.rfind("(@error");
    if (code != std::string::npos) line.erase(code);
    line = trimmed(line);
    if (line.empty() || isCaretLine(line)) continue;

    if (!scriptDir.empty() && line.size() > scriptDir.size() && line.compare(0, scriptDir.size(), scriptDir) == 0 &&
        (line[scriptDir.size()] == '/' || line[scriptDir.size()] == '\\'))
      line.erase(0, scriptDir.size() + 1);
    if (std::find(diagnostics.begin(), diagnostics.end(), line) == diagnostics.end()) diagnostics.push_back(line);
  }

  if (diagnostics.empty()) return {};
  constexpr size_t kShown = 3;
  std::string summary = diagnostics[0];
  for (size_t i = 1; i < diagnostics.size() && i < kShown; ++i) summary += "; " + diagnostics[i];
  if (diagnostics.size() > kShown) summary += " (+" + std::to_string(diagnostics.size() - kShown) + " more)";
  return summary;
}

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
  struct ParameterBinding {
    std::string id;
    Ctl::FunctionArgPtr arg;
    ParameterType type = ParameterType::Unsupported;
    ParameterValue value;
    ParameterValue defaultValue;
  };

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
  std::vector<ParameterBinding> exposedParameters;
  std::mutex mutex;

  static ParameterType exposedParameterType(const Ctl::FunctionArgPtr &arg) {
    if (!arg.refcount() || arg->isVarying()) return ParameterType::Unsupported;
    if (arg->type().cast<Ctl::FloatType>().refcount() != 0) return ParameterType::Double;
    if (arg->type().cast<Ctl::IntType>().refcount() != 0) return ParameterType::Integer;
    if (arg->type().cast<Ctl::BoolType>().refcount() != 0) return ParameterType::Boolean;
    return ParameterType::Unsupported;
  }

  static ParameterValue readValue(const Ctl::FunctionArgPtr &arg, ParameterType type) {
    switch (type) {
      case ParameterType::Double:
        return (double)*reinterpret_cast<const float *>(arg->data());
      case ParameterType::Integer:
        return *reinterpret_cast<const int *>(arg->data());
      case ParameterType::Boolean:
        return *reinterpret_cast<const unsigned char *>(arg->data()) != 0;
      default:
        return {};
    }
  }

  static bool normaliseValue(ParameterBinding &binding, const ParameterValue &value) {
    switch (binding.type) {
      case ParameterType::Double: {
        const double *v = std::get_if<double>(&value);
        if (!v) return false;
        binding.value = (double)(float)*v;
        return true;
      }
      case ParameterType::Integer: {
        const int *v = std::get_if<int>(&value);
        if (!v) return false;
        binding.value = *v;
        return true;
      }
      case ParameterType::Boolean: {
        const bool *v = std::get_if<bool>(&value);
        if (!v) return false;
        binding.value = *v;
        return true;
      }
      default:
        return false;
    }
  }

  static void writeValue(const ParameterBinding &binding) {
    switch (binding.type) {
      case ParameterType::Double:
        *reinterpret_cast<float *>(binding.arg->data()) =
            (float)std::get<double>(binding.value);
        break;
      case ParameterType::Integer:
        *reinterpret_cast<int *>(binding.arg->data()) =
            std::get<int>(binding.value);
        break;
      case ParameterType::Boolean:
        *reinterpret_cast<unsigned char *>(binding.arg->data()) =
            std::get<bool>(binding.value) ? 1 : 0;
        break;
      default:
        break;
    }
  }

  ParameterBinding *findParameter(const std::string &id) {
    for (auto &binding : exposedParameters)
      if (binding.id == id) return &binding;
    return nullptr;
  }

  void applyExposedParameters() {
    for (const auto &binding : exposedParameters) writeValue(binding);
  }

  void load(const std::string &path) {
    std::vector<std::string> modulePaths = Ctl::Interpreter::modulePaths();
    const std::string parent = fs::path(path).parent_path().string();
    if (!parent.empty() && std::find(modulePaths.begin(), modulePaths.end(), parent) == modulePaths.end())
      modulePaths.insert(modulePaths.begin(), parent);
    interpreter.setUserModulePath(modulePaths, true);

    interpreter.loadFile(path);
    function = interpreter.newFunctionCall("main");

    if (function->returnValue()->type().cast<Ctl::VoidType>().refcount() == 0)
      throw ContractError("CTL main() must return void");

    rIn = function->findInputArg("rIn");
    gIn = function->findInputArg("gIn");
    bIn = function->findInputArg("bIn");
    aIn = function->findInputArg("aIn");
    rOut = function->findOutputArg("rOut");
    gOut = function->findOutputArg("gOut");
    bOut = function->findOutputArg("bOut");
    aOut = function->findOutputArg("aOut");

    if (!validVaryingFloat(rIn) || !validVaryingFloat(gIn) || !validVaryingFloat(bIn))
      throw ContractError("CTL main() must provide varying float rIn, gIn and bIn inputs");
    if (!validVaryingFloat(rOut) || !validVaryingFloat(gOut) || !validVaryingFloat(bOut))
      throw ContractError("CTL main() must provide varying float rOut, gOut and bOut outputs");
    if (aIn.refcount() != 0 && !validVaryingFloat(aIn))
      throw ContractError("CTL aIn must be a varying float when present");
    if (aOut.refcount() != 0 && !validVaryingFloat(aOut))
      throw ContractError("CTL aOut must be a varying float when present");

    for (size_t i = 0; i < function->numInputArgs(); ++i) {
      Ctl::FunctionArgPtr arg = function->inputArg(i);
      const std::string &name = arg->name();
      if (name == "rIn" || name == "gIn" || name == "bIn" || name == "aIn") continue;
      if (!arg->hasDefaultValue())
        throw ContractError("Unsupported required CTL input parameter: " + name);

      // Plain CTL provides a type/name/default but no UI range metadata.
      // Defaulted scalar uniform float/int/bool inputs are therefore exposed
      // through RawNode's generic parameter API as unbounded controls.
      arg->setDefaultValue();
      const ParameterType type = exposedParameterType(arg);
      if (type == ParameterType::Unsupported) continue;

      ParameterBinding binding;
      binding.id = name;
      binding.arg = arg;
      binding.type = type;
      binding.defaultValue = readValue(arg, type);
      binding.value = binding.defaultValue;
      exposedParameters.push_back(std::move(binding));
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

  // Prefer CTL's own diagnostics over its generic load/lookup exception text.
  ScopedCtlMessageCapture capture;
  const auto ctlError = [&](const char *fallback) {
    const std::string summary = summarizeCtlMessages(capture.text(), fs::path(canonical).parent_path().string());
    return summary.empty() ? std::string(fallback) : summary;
  };

  try {
    auto impl = std::make_unique<Impl>();
    impl->load(canonical);
    std::string name = fs::path(canonical).stem().string();
    if (name.empty()) name = "CTL";
    return std::unique_ptr<CtlProcessor>(
        new CtlProcessor(canonical, std::move(name), std::move(impl)));
  } catch (const ContractError &e) {
    if (error) *error = e.what();
    return nullptr;
  } catch (const std::exception &e) {
    if (error) *error = ctlError(e.what());
    return nullptr;
  } catch (...) {
    if (error) *error = ctlError("Unknown CTL interpreter error");
    return nullptr;
  }
}

std::string CtlProcessor::identifier() const { return path_; }

std::string CtlProcessor::displayName() const { return name_; }

std::vector<ProcessorParameter> CtlProcessor::parameters() const {
  std::vector<ProcessorParameter> out;
  if (!impl_) return out;

  std::lock_guard<std::mutex> lock(impl_->mutex);
  out.reserve(impl_->exposedParameters.size());
  for (const auto &binding : impl_->exposedParameters) {
    ProcessorParameter param;
    param.id = binding.id;
    param.label = binding.id;
    param.hint = "Standard CTL input parameter";
    param.type = binding.type;
    param.value = binding.value;
    param.defaultValue = binding.defaultValue;
    param.hasRange = false;
    out.push_back(std::move(param));
  }
  return out;
}

bool CtlProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)notify;
  if (!impl_) return false;

  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::ParameterBinding *binding = impl_->findParameter(id);
  if (!binding || !Impl::normaliseValue(*binding, value)) return false;
  Impl::writeValue(*binding);
  return true;
}

bool CtlProcessor::resetParameter(const std::string &id, bool notify) {
  (void)notify;
  if (!impl_) return false;

  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::ParameterBinding *binding = impl_->findParameter(id);
  if (!binding) return false;
  binding->value = binding->defaultValue;
  Impl::writeValue(*binding);
  return true;
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

      // Re-apply uniform UI state for each SIMD chunk so processor state is
      // independent of any register changes made while executing the script.
      impl_->applyExposedParameters();

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
