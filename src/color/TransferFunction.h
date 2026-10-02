#pragma once

#include <string>

enum class TransferFunction {
  Linear = 0,
  SRGB = 1,
  Rec709 = 2,
  DaVinciIntermediate = 3,
};

struct TransferFunctionDefinition {
  TransferFunction value;
  const char *id;
  const char *name;
};

int transferFunctionCount();
const TransferFunctionDefinition &transferFunctionDefinition(int index);
const TransferFunctionDefinition &transferFunctionDefinition(TransferFunction tf);
int transferFunctionIndex(TransferFunction tf);
const char *transferFunctionId(TransferFunction tf);
const char *transferFunctionName(TransferFunction tf);
bool transferFunctionFromIdOrName(const std::string &name, TransferFunction &tf);

// Backwards-compatible name parser for PR #17 sidecars and older call sites.
inline bool transferFunctionFromName(const std::string &name, TransferFunction &tf) {
  return transferFunctionFromIdOrName(name, tf);
}

// Convert one channel between encoded and scene-linear light. The piecewise
// definitions intentionally keep negative values finite where their published
// linear segments permit it.
double decodeTransfer(double value, TransferFunction tf);
double encodeTransfer(double linear, TransferFunction tf);
