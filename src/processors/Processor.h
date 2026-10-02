#pragma once

#include "imgio/ImageIO.h"

#include <string>
#include <utility>

enum class ProcessorBackend {
  OFX,
  CTL,
  DCTL,
  Native,
};

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

  // Allows backends to update any size-dependent state before rendering.
  // The current app calls this from the UI thread before preview/export renders.
  virtual void setRenderSize(int width, int height) = 0;

  // Input/output are bottom-up float RGBA in whatever colour space the chain
  // has reached at this node. Input and output must be distinct Images.
  //
  // generation == 0 means "do not cancel". Non-zero values are currently used
  // by interactive preview rendering to abandon superseded work.
  virtual ProcessorResult render(const Image &input, Image &output, int generation) = 0;
};
