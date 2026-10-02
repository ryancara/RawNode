#pragma once

#include "imgio/ImageIO.h"

// Matrix-only transforms between RawNode's supported scene-linear RGB spaces.
// Non-linear ColorSpace values (sRGB / Display P3) are deliberately rejected:
// transfer functions belong to the CST processor as an explicit, separate step.
bool linearColorTransformMatrix(ColorSpace source, ColorSpace target, double out[3][3]);

// Applies a 3x3 RGB matrix. Alpha is intentionally outside this helper.
inline void applyLinearColorMatrix(const double matrix[3][3], const float rgbIn[3], float rgbOut[3]) {
  for (int row = 0; row < 3; ++row) {
    const double v = matrix[row][0] * rgbIn[0] +
                     matrix[row][1] * rgbIn[1] +
                     matrix[row][2] * rgbIn[2];
    rgbOut[row] = (float)v;
  }
}
