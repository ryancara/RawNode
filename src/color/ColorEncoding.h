#pragma once

#include "color/TransferFunction.h"

#include <string>

// Numeric values 0..4 are retained from the first PR #17 CST implementation so
// numeric sidecars written by that build keep their original meaning.
// Display P3 is appended rather than inserted for backwards compatibility.
enum class RgbGamut {
  Rec709 = 0,
  Rec2020 = 1,
  ACES_AP0 = 2,
  ACES_AP1 = 3,
  DaVinciWideGamut = 4,
  DisplayP3 = 5,
};

struct RgbGamutDefinition {
  RgbGamut value;
  const char *id;
  const char *name;
  double redX, redY;
  double greenX, greenY;
  double blueX, blueY;
  double whiteX, whiteY;
};

int rgbGamutCount();
const RgbGamutDefinition &rgbGamutDefinition(int index);
const RgbGamutDefinition &rgbGamutDefinition(RgbGamut gamut);
int rgbGamutIndex(RgbGamut gamut);
const char *rgbGamutId(RgbGamut gamut);
const char *rgbGamutName(RgbGamut gamut);
bool rgbGamutFromIdOrName(const std::string &name, RgbGamut &gamut);

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

bool colorEncodingFromIds(const std::string &gamutId, const std::string &transferId,
                          ColorEncoding &encoding);
std::string colorEncodingName(const ColorEncoding &encoding);
