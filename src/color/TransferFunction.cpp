#include "color/TransferFunction.h"

#include <cmath>

const char *transferFunctionName(TransferFunction tf) {
  switch (tf) {
    case TransferFunction::Linear: return "Linear";
    case TransferFunction::SRGB: return "sRGB";
    case TransferFunction::Rec709: return "Rec.709";
    case TransferFunction::DaVinciIntermediate: return "DaVinci Intermediate";
  }
  return "Linear";
}

double decodeTransfer(double value, TransferFunction tf) {
  switch (tf) {
    case TransferFunction::Linear:
      return value;

    case TransferFunction::SRGB:
      if (value <= 0.04045) return value / 12.92;
      return std::pow((value + 0.055) / 1.055, 2.4);

    case TransferFunction::Rec709:
      if (value < 0.081) return value / 4.5;
      return std::pow((value + 0.099) / 1.099, 1.0 / 0.45);

    case TransferFunction::DaVinciIntermediate: {
      constexpr double kA = 0.0075;
      constexpr double kB = 7.0;
      constexpr double kC = 0.07329248;
      constexpr double kM = 10.44426855;
      constexpr double kLogCut = 0.02740668;
      if (value <= kLogCut) return value / kM;
      return std::pow(2.0, value / kC - kB) - kA;
    }
  }
  return value;
}

double encodeTransfer(double linear, TransferFunction tf) {
  switch (tf) {
    case TransferFunction::Linear:
      return linear;

    case TransferFunction::SRGB:
      if (linear <= 0.0031308) return 12.92 * linear;
      return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;

    case TransferFunction::Rec709:
      if (linear < 0.018) return 4.5 * linear;
      return 1.099 * std::pow(linear, 0.45) - 0.099;

    case TransferFunction::DaVinciIntermediate: {
      constexpr double kA = 0.0075;
      constexpr double kB = 7.0;
      constexpr double kC = 0.07329248;
      constexpr double kM = 10.44426855;
      constexpr double kLinCut = 0.00262409;
      if (linear <= kLinCut) return linear * kM;
      return (std::log2(linear + kA) + kB) * kC;
    }
  }
  return linear;
}
