#include "color/LinearColorTransform.h"

#include <cmath>

namespace {

using Matrix3 = double[3][3];

constexpr Matrix3 kIdentity = {
    {1.0, 0.0, 0.0},
    {0.0, 1.0, 0.0},
    {0.0, 0.0, 1.0},
};

constexpr Matrix3 kBradford = {
    {0.8951, 0.2664, -0.1614},
    {-0.7502, 1.7135, 0.0367},
    {0.0389, -0.0685, 1.0296},
};

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

void multiply3x3Vector(const double m[3][3], const double v[3], double out[3]) {
  for (int row = 0; row < 3; ++row)
    out[row] = m[row][0] * v[0] + m[row][1] * v[1] + m[row][2] * v[2];
}

void xyToXyz(double x, double y, double out[3]) {
  out[0] = x / y;
  out[1] = 1.0;
  out[2] = (1.0 - x - y) / y;
}

bool rgbToXyzMatrix(RgbGamut gamut, double out[3][3]) {
  const auto &d = rgbGamutDefinition(gamut);
  double r[3], g[3], b[3], w[3];
  xyToXyz(d.redX, d.redY, r);
  xyToXyz(d.greenX, d.greenY, g);
  xyToXyz(d.blueX, d.blueY, b);
  xyToXyz(d.whiteX, d.whiteY, w);

  double primaries[3][3] = {
      {r[0], g[0], b[0]},
      {r[1], g[1], b[1]},
      {r[2], g[2], b[2]},
  };
  double inv[3][3] = {};
  if (!invert3x3(primaries, inv)) return false;

  double scale[3] = {};
  multiply3x3Vector(inv, w, scale);
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      out[row][col] = primaries[row][col] * scale[col];
  return true;
}

bool bradfordAdaptation(const RgbGamutDefinition &source, const RgbGamutDefinition &target,
                        double out[3][3]) {
  if (source.whiteX == target.whiteX && source.whiteY == target.whiteY) {
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        out[row][col] = kIdentity[row][col];
    return true;
  }

  double srcWhite[3], dstWhite[3];
  xyToXyz(source.whiteX, source.whiteY, srcWhite);
  xyToXyz(target.whiteX, target.whiteY, dstWhite);

  double srcCone[3], dstCone[3];
  multiply3x3Vector(kBradford, srcWhite, srcCone);
  multiply3x3Vector(kBradford, dstWhite, dstCone);
  if (std::fabs(srcCone[0]) < 1e-15 || std::fabs(srcCone[1]) < 1e-15 ||
      std::fabs(srcCone[2]) < 1e-15)
    return false;

  double invBradford[3][3] = {};
  if (!invert3x3(kBradford, invBradford)) return false;

  double scaledBradford[3][3] = {};
  // Bradford adaptation is B^-1 * diag(dstCone/srcCone) * B, so the
  // diagonal matrix scales rows of B, not columns.
  for (int row = 0; row < 3; ++row) {
    const double scale = dstCone[row] / srcCone[row];
    for (int col = 0; col < 3; ++col)
      scaledBradford[row][col] = kBradford[row][col] * scale;
  }
  multiply3x3(invBradford, scaledBradford, out);
  return true;
}

}  // namespace

bool linearColorTransformMatrix(RgbGamut source, RgbGamut target, double out[3][3]) {
  if (source == target) {
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        out[row][col] = kIdentity[row][col];
    return true;
  }

  double sourceToXyz[3][3] = {};
  double targetToXyz[3][3] = {};
  if (!rgbToXyzMatrix(source, sourceToXyz) || !rgbToXyzMatrix(target, targetToXyz))
    return false;

  double xyzToTarget[3][3] = {};
  if (!invert3x3(targetToXyz, xyzToTarget)) return false;

  double adaptation[3][3] = {};
  if (!bradfordAdaptation(rgbGamutDefinition(source), rgbGamutDefinition(target), adaptation))
    return false;

  double adaptedSource[3][3] = {};
  multiply3x3(adaptation, sourceToXyz, adaptedSource);
  multiply3x3(xyzToTarget, adaptedSource, out);
  return true;
}

