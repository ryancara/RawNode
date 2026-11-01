#pragma once

#include "ofxImageEffect.h"

// Optional RawNode extension, not an OpenFX standard suite. Call once from the
// Render calling thread, using the same geometry as those pixels. Six doubles:
// xx,xy,tx,yx,yy,ty in normalized lower-left input/output raster coordinates.
// nullptr means explicit Unknown; no call means undeclared. No effect properties
// are mutated; callback access ends with the Render action.
inline constexpr const char *kRawNodeOfxCoordinateMappingSuite = "RawNodeOfxCoordinateMappingSuite";
struct RawNodeOfxCoordinateMappingSuiteV1 {
  OfxStatus (*setInputToOutput)(OfxImageEffectHandle effect, const double *affine);
};
