#include "color/LinearColorTransform.h"

#include <cmath>

namespace {

using Matrix3 = double[3][3];

constexpr Matrix3 kIdentity = {
    {1.0, 0.0, 0.0},
    {0.0, 1.0, 0.0},
    {0.0, 0.0, 1.0},
};

// Linear Rec.709/sRGB D65 -> Linear Rec.2020 D65.
constexpr Matrix3 kRec709ToRec2020 = {
    {0.627403895934699, 0.329283038377883, 0.043313065687418},
    {0.069097289358232, 0.919540395075459, 0.011362315566309},
    {0.016391438875150, 0.088013307877226, 0.895595253247624},
};

// Linear Rec.709/sRGB D65 -> ACES2065-1/AP0 D60.
// This is the inverse of the AP0 -> Linear Rec.709 matrix used by the
// ACES/OCIO reference configuration, so the D65 <-> D60 adaptation is part
// of the matrix rather than a separate hidden operation.
constexpr Matrix3 kRec709ToAcesAp0 = {
    {0.439632981919492, 0.382988698151554, 0.177378319928956},
    {0.089776442958842, 0.813439428748978, 0.096784128292177},
    {0.017541170383173, 0.111546553302387, 0.870912276314442},
};

// Rec.709 D65 -> ACES AP1 D60. Derived from the reviewed Rec.709 -> AP0
// transform above followed by the ACES reference AP0 -> AP1 matrix.
constexpr Matrix3 kRec709ToAcesAp1 = {
    {0.613097402379707, 0.339523146156163, 0.047379451364133},
    {0.070193722465476, 0.916353879032696, 0.013452398501823},
    {0.020615592870732, 0.109569772924858, 0.869814634204413},
};

// Rec.709 D65 -> DaVinci Wide Gamut D65. Derived from Blackmagic Design's
// published DWG primaries/white point (v1.1) and the standard Rec.709 D65
// primaries. Both spaces are D65, so no chromatic adaptation is required.
constexpr Matrix3 kRec709ToDwg = {
    {0.562767456007108, 0.323516588703959, 0.113715955288933},
    {0.077754635285046, 0.749577346163222, 0.172668018551732},
    {0.064669199916328, 0.191998692046299, 0.743332108037373},
};

const double (*rec709To(RgbGamut gamut))[3] {
  switch (gamut) {
    case RgbGamut::Rec709: return kIdentity;
    case RgbGamut::Rec2020: return kRec709ToRec2020;
    case RgbGamut::ACES_AP0: return kRec709ToAcesAp0;
    case RgbGamut::ACES_AP1: return kRec709ToAcesAp1;
    case RgbGamut::DaVinciWideGamut: return kRec709ToDwg;
  }
  return nullptr;
}

bool colorSpaceToGamut(ColorSpace space, RgbGamut &gamut) {
  switch (space) {
    case ColorSpace::LinearRec709:
      gamut = RgbGamut::Rec709;
      return true;
    case ColorSpace::LinearRec2020:
      gamut = RgbGamut::Rec2020;
      return true;
    case ColorSpace::ACES2065_1:
      gamut = RgbGamut::ACES_AP0;
      return true;
    case ColorSpace::sRGB:
    case ColorSpace::DisplayP3:
      return false;
  }
  return false;
}

bool invert3x3(const double m[3][3], double out[3][3]) {
  const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
  const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
  const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
  if (!std::isfinite(det) || std::fabs(det) < 1e-15) return false;

  const double invDet = 1.0 / det;
  out[0][0] = c00 * invDet;
  out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * invDet;
  out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * invDet;
  out[1][0] = c01 * invDet;
  out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * invDet;
  out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * invDet;
  out[2][0] = c02 * invDet;
  out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * invDet;
  out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * invDet;
  return true;
}

void multiply3x3(const double a[3][3], const double b[3][3], double out[3][3]) {
  double tmp[3][3] = {};
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      for (int k = 0; k < 3; ++k)
        tmp[row][col] += a[row][k] * b[k][col];

  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      out[row][col] = tmp[row][col];
}

}  // namespace

const char *rgbGamutName(RgbGamut gamut) {
  switch (gamut) {
    case RgbGamut::Rec709: return "Rec.709";
    case RgbGamut::Rec2020: return "Rec.2020";
    case RgbGamut::ACES_AP0: return "ACES AP0";
    case RgbGamut::ACES_AP1: return "ACES AP1";
    case RgbGamut::DaVinciWideGamut: return "DaVinci Wide Gamut";
  }
  return "Rec.709";
}

bool rgbGamutFromName(const std::string &name, RgbGamut &gamut) {
  if (name == "Rec.709" || name == "Rec.709 / sRGB") {
    gamut = RgbGamut::Rec709;
    return true;
  }
  if (name == "Rec.2020") {
    gamut = RgbGamut::Rec2020;
    return true;
  }
  if (name == "ACES AP0" || name == "ACES2065-1" || name == "ACES2065-1 (AP0)" || name == "AP0") {
    gamut = RgbGamut::ACES_AP0;
    return true;
  }
  if (name == "ACES AP1" || name == "ACEScg" || name == "AP1") {
    gamut = RgbGamut::ACES_AP1;
    return true;
  }
  if (name == "DaVinci Wide Gamut" || name == "DWG") {
    gamut = RgbGamut::DaVinciWideGamut;
    return true;
  }
  return false;
}

bool linearColorTransformMatrix(RgbGamut source, RgbGamut target, double out[3][3]) {
  const double (*rec709ToSource)[3] = rec709To(source);
  const double (*rec709ToTarget)[3] = rec709To(target);
  if (!rec709ToSource || !rec709ToTarget) return false;

  if (source == target) {
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        out[row][col] = kIdentity[row][col];
    return true;
  }

  double sourceToRec709[3][3] = {};
  if (!invert3x3(rec709ToSource, sourceToRec709)) return false;
  multiply3x3(rec709ToTarget, sourceToRec709, out);
  return true;
}

bool linearColorTransformMatrix(ColorSpace source, ColorSpace target, double out[3][3]) {
  RgbGamut sourceGamut, targetGamut;
  if (!colorSpaceToGamut(source, sourceGamut) || !colorSpaceToGamut(target, targetGamut)) return false;
  return linearColorTransformMatrix(sourceGamut, targetGamut, out);
}
