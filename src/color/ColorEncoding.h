#pragma once

#include "color/TransferFunction.h"

enum class RgbGamut {
  Rec709 = 0,
  Rec2020,
  ACES_AP0,
  ACES_AP1,
  DaVinciWideGamut,
};

struct ColorEncoding {
  RgbGamut gamut = RgbGamut::Rec2020;
  TransferFunction gamma = TransferFunction::Linear;
};

inline bool operator==(const ColorEncoding &a, const ColorEncoding &b) {
  return a.gamut == b.gamut && a.gamma == b.gamma;
}

inline bool operator!=(const ColorEncoding &a, const ColorEncoding &b) {
  return !(a == b);
}
