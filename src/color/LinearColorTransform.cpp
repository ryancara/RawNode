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
constexpr Matrix3 kRec709ToAces2065 = {
    {0.439632981919492, 0.382988698151554, 0.177378319928956},
    {0.089776442958842, 0.813439428748978, 0.096784128292177},
    {0.017541170383173, 0.111546553302387, 0.870912276314442},
};

const double (*rec709To(ColorSpace space))[3] {
  switch (space) {
    case ColorSpace::LinearRec709: return kIdentity;
    case ColorSpace::LinearRec2020: return kRec709ToRec2020;
    case ColorSpace::ACES2065_1: return kRec709ToAces2065;
    default: return nullptr;
  }
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

bool linearColorTransformMatrix(ColorSpace source, ColorSpace target, double out[3][3]) {
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
