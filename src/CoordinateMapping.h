#pragma once

#include <optional>
#include <utility>

// Correspondence between two raster-local frames. Coordinates are fractions of
// width/height from the lower-left raster edge; pixel centres are (i+.5)/size.
// This is metadata only: no clipping, resampling or global/source frame.
class CoordinateMapping {
 public:
  enum class Kind { Identity, Affine, Unknown };

  static CoordinateMapping identity();
  // x' = xx*x + xy*y + tx; y' = yx*x + yy*y + ty.
  // Non-finite coefficients become Unknown; exact identity is canonicalized.
  static CoordinateMapping affine(double xx, double xy, double tx,
                                  double yx, double yy, double ty);
  static CoordinateMapping unknown() { return {}; }

  Kind kind() const { return kind_; }
  // Apply this mapping first, then next. Unknown correspondence propagates.
  CoordinateMapping then(const CoordinateMapping &next) const;
  CoordinateMapping inverse() const;  // Singular/non-finite inverse is Unknown.
  std::optional<std::pair<double, double>> map(double x, double y) const;

 private:
  Kind kind_ = Kind::Unknown;
  double xx_ = 1, xy_ = 0, tx_ = 0, yx_ = 0, yy_ = 1, ty_ = 0;
};
