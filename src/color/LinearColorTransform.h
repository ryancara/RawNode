#pragma once

#include "imgio/ImageIO.h"

enum class RgbGamut {
  Rec709 = 0,
  Rec2020,
  ACES_AP0,
  ACES_AP1,
  DaVinciWideGamut,
};

const char *rgbGamutName(RgbGamut gamut);

// Matrix-only transforms between supported RGB primary sets. Transfer
// functions are deliberately handled separately by the CST processor.
bool linearColorTransformMatrix(RgbGamut source, RgbGamut target, double out[3][3]);

// Convenience overload used by RAW decode for the ColorSpace values that map
// to scene-linear RGB gamuts. Non-linear ColorSpace values are rejected.
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
