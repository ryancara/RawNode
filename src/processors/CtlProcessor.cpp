#include "processors/CtlProcessor.h"

#include <CtlFunctionCall.h>
#include <CtlInterpreter.h>
#include <CtlMessage.h>
#include <CtlSimdInterpreter.h>
#include <CtlStdType.h>
#include <CtlType.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
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
  // Fixed after load(): which CTL inputs are exposed and how.
  struct ParameterBinding {
    std::string id;
    Ctl::FunctionArgPtr arg;
    ParameterType type = ParameterType::Unsupported;
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
  bool artDialect = false;

  // Two locks, never held together:
  // - parameterMutex guards parameterValues, the UI-facing state (parallel to
  //   exposedParameters). It is held only briefly, so parameter reads and edits
  //   never wait for a CTL render.
  // - renderMutex guards the interpreter and its argument registers for the
  //   whole of a render.
  std::mutex parameterMutex;
  std::vector<ParameterValue> parameterValues;
  std::mutex renderMutex;

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
        return *reinterpret_cast<const bool *>(arg->data());
      default:
        return {};
    }
  }

  static bool normaliseValue(ParameterType type, const ParameterValue &value, ParameterValue &out) {
    switch (type) {
      case ParameterType::Double: {
        // CTL float inputs are 32-bit. Reject NaN/inf and values that would
        // overflow (converting an out-of-range double to float is undefined).
        const double *v = std::get_if<double>(&value);
        if (!v || !std::isfinite(*v) || std::fabs(*v) > std::numeric_limits<float>::max()) return false;
        out = (double)(float)*v;
        return true;
      }
      case ParameterType::Integer: {
        const int *v = std::get_if<int>(&value);
        if (!v) return false;
        out = *v;
        return true;
      }
      case ParameterType::Boolean: {
        const bool *v = std::get_if<bool>(&value);
        if (!v) return false;
        out = *v;
        return true;
      }
      default:
        return false;
    }
  }

  static void writeValue(const ParameterBinding &binding, const ParameterValue &value) {
    switch (binding.type) {
      case ParameterType::Double:
        *reinterpret_cast<float *>(binding.arg->data()) = (float)std::get<double>(value);
        break;
      case ParameterType::Integer:
        *reinterpret_cast<int *>(binding.arg->data()) = std::get<int>(value);
        break;
      case ParameterType::Boolean:
        *reinterpret_cast<bool *>(binding.arg->data()) = std::get<bool>(value);
        break;
      default:
        break;
    }
  }

  int findParameter(const std::string &id) const {
    for (size_t i = 0; i < exposedParameters.size(); ++i)
      if (exposedParameters[i].id == id) return (int)i;
    return -1;
  }

  void load(const std::string &path) {
    std::vector<std::string> modulePaths = Ctl::Interpreter::modulePaths();
    const std::string parent = fs::path(path).parent_path().string();
    if (!parent.empty() && std::find(modulePaths.begin(), modulePaths.end(), parent) == modulePaths.end())
      modulePaths.insert(modulePaths.begin(), parent);
    interpreter.setUserModulePath(modulePaths, true);

    interpreter.loadFile(path);

    // Standard CTL remains the primary contract. If the module does not define
    // main(), fall back to ART's documented ART_main entry point. Other errors
    // creating main() are not hidden by the compatibility fallback.
    try {
      function = interpreter.newFunctionCall("main");
    } catch (const std::exception &e) {
      if (std::string(e.what()) != "Cannot find CTL function main.") throw;
      function = interpreter.newFunctionCall("ART_main");
      artDialect = true;
    }

    if (function->returnValue()->type().cast<Ctl::VoidType>().refcount() == 0)
      throw ContractError(artDialect ? "ART_main() must return void" : "CTL main() must return void");

    if (artDialect) {
      // ART defines the first three inputs/outputs positionally as varying
      // float RGB channels; parameter names after them are script-defined.
      if (function->numInputArgs() < 3 || function->numOutputArgs() < 3)
        throw ContractError("ART_main() must provide three varying float RGB inputs and outputs");

      rIn = function->inputArg(0);
      gIn = function->inputArg(1);
      bIn = function->inputArg(2);
      rOut = function->outputArg(0);
      gOut = function->outputArg(1);
      bOut = function->outputArg(2);

      if (!validVaryingFloat(rIn) || !validVaryingFloat(gIn) || !validVaryingFloat(bIn) ||
          !validVaryingFloat(rOut) || !validVaryingFloat(gOut) || !validVaryingFloat(bOut))
        throw ContractError("ART_main() RGB inputs and outputs must be varying float");

      // ART parameters may omit CTL defaults; ART specifies zero as the final
      // fallback. Metadata defaults/ranges/labels are added by the next adapter
      // step, but scalar float/int/bool parameters are executable now.
      for (size_t i = 3; i < function->numInputArgs(); ++i) {
        Ctl::FunctionArgPtr arg = function->inputArg(i);
        const ParameterType type = exposedParameterType(arg);
        if (type == ParameterType::Unsupported)
          throw ContractError("Unsupported ART CTL parameter type: " + arg->name());

        ParameterBinding binding;
        binding.id = arg->name();
        binding.arg = arg;
        binding.type = type;

        if (arg->hasDefaultValue()) {
          arg->setDefaultValue();
          binding.defaultValue = readValue(arg, type);
        } else {
          switch (type) {
            case ParameterType::Double: binding.defaultValue = 0.0; break;
            case ParameterType::Integer: binding.defaultValue = 0; break;
            case ParameterType::Boolean: binding.defaultValue = false; break;
            default: break;
          }
        }

        parameterValues.push_back(binding.defaultValue);
        exposedParameters.push_back(std::move(binding));
      }
    } else {
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
        parameterValues.push_back(binding.defaultValue);
        exposedParameters.push_back(std::move(binding));
      }

      if (aIn.refcount() != 0 && aIn->hasDefaultValue()) aIn->setDefaultValue();
    }
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

  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  out.reserve(impl_->exposedParameters.size());
  for (size_t i = 0; i < impl_->exposedParameters.size(); ++i) {
    const auto &binding = impl_->exposedParameters[i];
    ProcessorParameter param;
    param.id = binding.id;
    param.label = binding.id;
    param.hint = "Standard CTL input parameter";
    param.type = binding.type;
    param.value = impl_->parameterValues[i];
    param.defaultValue = binding.defaultValue;
    param.hasRange = false;
    out.push_back(std::move(param));
  }
  return out;
}

bool CtlProcessor::setParameterValue(const std::string &id, const ParameterValue &value, bool notify) {
  (void)notify;
  if (!impl_) return false;

  const int index = impl_->findParameter(id);
  if (index < 0) return false;
  ParameterValue normalised;
  if (!Impl::normaliseValue(impl_->exposedParameters[index].type, value, normalised)) return false;

  // Takes effect from the next render; never waits for one in progress.
  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  impl_->parameterValues[index] = std::move(normalised);
  return true;
}

bool CtlProcessor::resetParameter(const std::string &id, bool notify) {
  (void)notify;
  if (!impl_) return false;

  const int index = impl_->findParameter(id);
  if (index < 0) return false;

  std::lock_guard<std::mutex> lock(impl_->parameterMutex);
  impl_->parameterValues[index] = impl_->exposedParameters[index].defaultValue;
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

  std::vector<ParameterValue> values;
  {
    std::lock_guard<std::mutex> lock(impl_->parameterMutex);
    values = impl_->parameterValues;
  }

  std::lock_guard<std::mutex> lock(impl_->renderMutex);
  try {
    // One snapshot per render keeps every chunk of the frame consistent. CTL
    // rejects assignments to input parameters, so the script cannot change
    // these registers and they need no per-chunk re-application.
    for (size_t i = 0; i < impl_->exposedParameters.size(); ++i)
      Impl::writeValue(impl_->exposedParameters[i], values[i]);

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
