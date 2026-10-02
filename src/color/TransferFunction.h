#pragma once

#include <string>

enum class TransferFunction {
  Linear = 0,
  SRGB,
  Rec709,
  DaVinciIntermediate,
};

const char *transferFunctionName(TransferFunction tf);
bool transferFunctionFromName(const std::string &name, TransferFunction &tf);

// Convert one channel between encoded and scene-linear light. The piecewise
// definitions intentionally keep negative values finite where their published
// linear segments permit it.
double decodeTransfer(double value, TransferFunction tf);
double encodeTransfer(double linear, TransferFunction tf);
