#pragma once

#include "color/ColorEncoding.h"

bool linearColorTransformMatrix(RgbGamut source, RgbGamut target, double out[3][3]);


inline void applyLinearColorMatrix(const double matrix[3][3], const float rgbIn[3], float rgbOut[3]) {
  for (int row = 0; row < 3; ++row) {
    const double v = matrix[row][0] * rgbIn[0] +
                     matrix[row][1] * rgbIn[1] +
                     matrix[row][2] * rgbIn[2];
    rgbOut[row] = (float)v;
  }
}
