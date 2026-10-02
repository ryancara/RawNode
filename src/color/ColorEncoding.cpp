#include "color/ColorEncoding.h"

#include <array>

namespace {

// Registry order is presentation order, not enum order. Enum values 0..4 remain
// frozen for compatibility with numeric CST sidecars from early PR #17 builds.
constexpr std::array<RgbGamutDefinition, 6> kGamuts = {{
    {RgbGamut::Rec709, "rec709", "Rec.709",
     0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290},
    {RgbGamut::Rec2020, "rec2020", "Rec.2020",
     0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290},
    {RgbGamut::DisplayP3, "display-p3", "Display P3",
     0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290},
    {RgbGamut::ACES_AP0, "aces-ap0", "ACES AP0",
     0.73470, 0.26530, 0.00000, 1.00000, 0.00010, -0.07700, 0.32168, 0.33767},
    {RgbGamut::ACES_AP1, "aces-ap1", "ACES AP1",
     0.713, 0.293, 0.165, 0.830, 0.128, 0.044, 0.32168, 0.33767},
    {RgbGamut::DaVinciWideGamut, "davinci-wide-gamut", "DaVinci Wide Gamut",
     0.8000, 0.3130, 0.1682, 0.9877, 0.0790, -0.1155, 0.3127, 0.3290},
}};

}  // namespace

int rgbGamutCount() { return (int)kGamuts.size(); }

const RgbGamutDefinition &rgbGamutDefinition(int index) {
  if (index < 0 || index >= rgbGamutCount()) return kGamuts[0];
  return kGamuts[(size_t)index];
}

const RgbGamutDefinition &rgbGamutDefinition(RgbGamut gamut) {
  for (const auto &def : kGamuts)
    if (def.value == gamut) return def;
  return kGamuts[0];
}

int rgbGamutIndex(RgbGamut gamut) {
  for (int i = 0; i < rgbGamutCount(); ++i)
    if (kGamuts[(size_t)i].value == gamut) return i;
  return -1;
}

const char *rgbGamutId(RgbGamut gamut) { return rgbGamutDefinition(gamut).id; }
const char *rgbGamutName(RgbGamut gamut) { return rgbGamutDefinition(gamut).name; }

bool rgbGamutFromIdOrName(const std::string &name, RgbGamut &gamut) {
  for (const auto &def : kGamuts) {
    if (name == def.id || name == def.name) {
      gamut = def.value;
      return true;
    }
  }

  // PR #16/#17 and common aliases.
  if (name == "Rec.709 / sRGB") {
    gamut = RgbGamut::Rec709;
    return true;
  }
  if (name == "ACES2065-1" || name == "ACES2065-1 (AP0)" || name == "AP0") {
    gamut = RgbGamut::ACES_AP0;
    return true;
  }
  if (name == "ACEScg" || name == "AP1") {
    gamut = RgbGamut::ACES_AP1;
    return true;
  }
  if (name == "DWG") {
    gamut = RgbGamut::DaVinciWideGamut;
    return true;
  }
  return false;
}

std::string colorEncodingName(const ColorEncoding &encoding) {
  if (encoding.gamma == TransferFunction::Linear) {
    switch (encoding.gamut) {
      case RgbGamut::Rec709: return "Linear Rec.709";
      case RgbGamut::Rec2020: return "Linear Rec.2020";
      case RgbGamut::ACES_AP0: return "ACES2065-1";
      case RgbGamut::ACES_AP1: return "ACEScg";
      case RgbGamut::DaVinciWideGamut: return "Linear DaVinci Wide Gamut";
      case RgbGamut::DisplayP3: return "Linear Display P3";
    }
  }
  return std::string(rgbGamutName(encoding.gamut)) + " / " + transferFunctionName(encoding.gamma);
}

const char *colorSpaceName(ColorSpace cs) {
  switch (cs) {
    case ColorSpace::sRGB: return "sRGB";
    case ColorSpace::DisplayP3: return "Display P3";
    case ColorSpace::LinearRec709: return "Linear Rec.709";
    case ColorSpace::LinearRec2020: return "Linear Rec.2020";
    case ColorSpace::ACES2065_1: return "ACES2065-1";
  }
  return "sRGB";
}

bool colorSpaceFromName(const std::string &name, ColorSpace &cs) {
  for (ColorSpace candidate : {ColorSpace::sRGB, ColorSpace::DisplayP3, ColorSpace::LinearRec709,
                               ColorSpace::LinearRec2020, ColorSpace::ACES2065_1}) {
    if (name == colorSpaceName(candidate)) {
      cs = candidate;
      return true;
    }
  }
  if (name == "ACES2065-1 (AP0)" || name == "AP0") {
    cs = ColorSpace::ACES2065_1;
    return true;
  }
  return false;
}

ColorEncoding legacyColorSpaceEncoding(ColorSpace cs) {
  switch (cs) {
    case ColorSpace::sRGB:
      return {RgbGamut::Rec709, TransferFunction::SRGB};
    case ColorSpace::DisplayP3:
      return {RgbGamut::DisplayP3, TransferFunction::SRGB};
    case ColorSpace::LinearRec709:
      return {RgbGamut::Rec709, TransferFunction::Linear};
    case ColorSpace::LinearRec2020:
      return {RgbGamut::Rec2020, TransferFunction::Linear};
    case ColorSpace::ACES2065_1:
      return {RgbGamut::ACES_AP0, TransferFunction::Linear};
  }
  return {RgbGamut::Rec709, TransferFunction::SRGB};
}

bool legacyColorSpaceFromEncoding(const ColorEncoding &encoding, ColorSpace &cs) {
  for (ColorSpace candidate : {ColorSpace::sRGB, ColorSpace::DisplayP3, ColorSpace::LinearRec709,
                               ColorSpace::LinearRec2020, ColorSpace::ACES2065_1}) {
    if (legacyColorSpaceEncoding(candidate) == encoding) {
      cs = candidate;
      return true;
    }
  }
  return false;
}

bool isRawWorkingSpace(ColorSpace cs) {
  return cs == ColorSpace::LinearRec709 || cs == ColorSpace::LinearRec2020 ||
         cs == ColorSpace::ACES2065_1;
}
