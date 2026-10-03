#include "color/TransferFunction.h"

#include <array>
#include <cmath>

namespace {

constexpr std::array<TransferFunctionDefinition, 4> kTransferFunctions = {{
    {TransferFunction::Linear, "linear", "Linear"},
    {TransferFunction::SRGB, "srgb", "sRGB"},
    {TransferFunction::Rec709, "rec709-camera", "Rec.709 (camera)"},
    {TransferFunction::DaVinciIntermediate, "davinci-intermediate", "DaVinci Intermediate"},
}};

}  // namespace

int transferFunctionCount() { return (int)kTransferFunctions.size(); }

const TransferFunctionDefinition &transferFunctionDefinition(int index) {
  if (index < 0 || index >= transferFunctionCount()) return kTransferFunctions[0];
  return kTransferFunctions[(size_t)index];
}

const TransferFunctionDefinition &transferFunctionDefinition(TransferFunction tf) {
  for (const auto &def : kTransferFunctions)
    if (def.value == tf) return def;
  return kTransferFunctions[0];
}

int transferFunctionIndex(TransferFunction tf) {
  for (int i = 0; i < transferFunctionCount(); ++i)
    if (kTransferFunctions[(size_t)i].value == tf) return i;
  return -1;
}

const char *transferFunctionId(TransferFunction tf) {
  return transferFunctionDefinition(tf).id;
}

const char *transferFunctionName(TransferFunction tf) {
  return transferFunctionDefinition(tf).name;
}

bool transferFunctionFromId(const std::string &id, TransferFunction &tf) {
  for (const auto &def : kTransferFunctions) {
    if (id == def.id) {
      tf = def.value;
      return true;
    }
  }
  return false;
}

double decodeTransfer(double value, TransferFunction tf) {
  switch (tf) {
    case TransferFunction::Linear:
      return value;

    case TransferFunction::SRGB:
      if (value <= 0.04045) return value / 12.92;
      return std::pow((value + 0.055) / 1.055, 2.4);

    case TransferFunction::Rec709: {
      // Exact BT.709 OETF constants. The rounded 1.099/0.018 form introduces
      // a small discontinuity and a locally non-monotonic inverse.
      constexpr double kAlpha = 1.09929682680944;
      constexpr double kBeta = 0.018053968510807;
      constexpr double kEncodedCut = 4.5 * kBeta;
      if (value < kEncodedCut) return value / 4.5;
      return std::pow((value + (kAlpha - 1.0)) / kAlpha, 1.0 / 0.45);
    }

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

    case TransferFunction::Rec709: {
      constexpr double kAlpha = 1.09929682680944;
      constexpr double kBeta = 0.018053968510807;
      if (linear < kBeta) return 4.5 * linear;
      return kAlpha * std::pow(linear, 0.45) - (kAlpha - 1.0);
    }

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
