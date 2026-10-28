#pragma once

#include <vector>

struct Mask {
  std::vector<float> px;  // One scalar coverage sample per pixel, bottom-up.
  int w = 0, h = 0;
};
