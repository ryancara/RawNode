#include "CoordinateMapping.h"

#include <cmath>

CoordinateMapping CoordinateMapping::identity() {
  CoordinateMapping result;
  result.kind_ = Kind::Identity;
  return result;
}

CoordinateMapping CoordinateMapping::affine(double xx, double xy, double tx,
                                            double yx, double yy, double ty) {
  if (!std::isfinite(xx) || !std::isfinite(xy) || !std::isfinite(tx) ||
      !std::isfinite(yx) || !std::isfinite(yy) || !std::isfinite(ty)) return unknown();
  CoordinateMapping result;
  result.kind_ = (xx == 1 && xy == 0 && tx == 0 && yx == 0 && yy == 1 && ty == 0)
      ? Kind::Identity : Kind::Affine;
  result.xx_ = xx; result.xy_ = xy; result.tx_ = tx;
  result.yx_ = yx; result.yy_ = yy; result.ty_ = ty;
  return result;
}

CoordinateMapping CoordinateMapping::then(const CoordinateMapping &next) const {
  if (kind_ == Kind::Unknown || next.kind_ == Kind::Unknown) return unknown();
  return affine(next.xx_ * xx_ + next.xy_ * yx_,
                next.xx_ * xy_ + next.xy_ * yy_,
                next.xx_ * tx_ + next.xy_ * ty_ + next.tx_,
                next.yx_ * xx_ + next.yy_ * yx_,
                next.yx_ * xy_ + next.yy_ * yy_,
                next.yx_ * tx_ + next.yy_ * ty_ + next.ty_);
}

CoordinateMapping CoordinateMapping::inverse() const {
  if (kind_ == Kind::Unknown) return unknown();
  const double determinant = xx_ * yy_ - xy_ * yx_;
  if (determinant == 0 || !std::isfinite(determinant)) return unknown();
  return affine(yy_ / determinant, -xy_ / determinant, (xy_ * ty_ - yy_ * tx_) / determinant,
                -yx_ / determinant, xx_ / determinant, (yx_ * tx_ - xx_ * ty_) / determinant);
}

std::optional<std::pair<double, double>> CoordinateMapping::map(double x, double y) const {
  if (kind_ == Kind::Unknown || !std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
  const double mappedX = xx_ * x + xy_ * y + tx_;
  const double mappedY = yx_ * x + yy_ * y + ty_;
  if (!std::isfinite(mappedX) || !std::isfinite(mappedY)) return std::nullopt;
  return std::pair<double, double>{mappedX, mappedY};
}
