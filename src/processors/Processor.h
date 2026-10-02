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
  virtual void setRenderSize(int width, int height) = 0;

  // Input and output are scene-linear float RGBA Images. Backends own any
  // backend-specific details such as output-size queries or GPU dispatch.
  virtual ProcessorResult render(const Image &input, Image &output, int generation) = 0;
};
