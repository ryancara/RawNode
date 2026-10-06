#pragma once

#include "imgio/ImageIO.h"
#include "processors/Parameter.h"
#include "RenderCancellation.h"

#include <string>
#include <utility>

enum class ProcessorBackend {
  OFX,
  CTL,
  DCTL,
  Native,
};

inline const char *processorBackendName(ProcessorBackend backend) {
  switch (backend) {
    case ProcessorBackend::OFX: return "ofx";
    case ProcessorBackend::CTL: return "ctl";
    case ProcessorBackend::DCTL: return "dctl";
    case ProcessorBackend::Native: return "native";
  }
  return "unknown";
}

struct ProcessorResult {
  bool ok = true;
  int backendCode = 0;
  std::string message;

  static ProcessorResult success() { return {}; }
  static ProcessorResult failure(int code, std::string message) {
    ProcessorResult r;
    r.ok = false;
    r.backendCode = code;
    r.message = std::move(message);
    return r;
  }
};

class Processor {
 public:
  virtual ~Processor() = default;

  virtual ProcessorBackend backend() const = 0;
  virtual std::string identifier() const = 0;
  virtual std::string displayName() const = 0;

  // Parameter metadata and values are exposed through a backend-neutral
  // snapshot. Writes use stable parameter IDs and the backend is responsible
  // for any change notification its runtime requires.
  virtual std::vector<ProcessorParameter> parameters() const = 0;
  virtual bool setParameterValue(const std::string &id, const ParameterValue &value, bool notify = true) = 0;
  virtual bool resetParameter(const std::string &id, bool notify = true) = 0;
  virtual bool activateParameter(const std::string &id) = 0;

  // Allows backends to update any size-dependent state before rendering.
  // The app calls this only while it has exclusive render ownership.
  virtual void setRenderSize(int width, int height) = 0;

  // Input/output are bottom-up float RGBA in whatever colour space the chain
  // has reached at this node. Input and output must be distinct Images.
  //
  // The borrowed token allows cooperative cancellation of obsolete previews.
  // An empty token (export) never cancels; no scheduler state belongs here.
  virtual ProcessorResult render(const Image &input, Image &output, const RenderCancellation &cancellation) = 0;
};
