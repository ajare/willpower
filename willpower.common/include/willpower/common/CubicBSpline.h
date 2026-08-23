#pragma once

#include <algorithm>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#include <spline_library/splines/uniform_cubic_bspline.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "willpower/common/Platform.h"
#include "willpower/common/Vector2.h"
#include "willpower/common/SplinePath.h"

namespace WP_NAMESPACE {

class WP_COMMON_API CubicBSpline : public SplinePath {
  UniformCubicBSpline<Vector2>* mSpline;

  float mLength, mMaxT;

public:
  explicit CubicBSpline(std::vector<wp::Vector2> const& points);

  ~CubicBSpline();

  void setControlPoint(int index, Vector2 const& position);

  Vector2 getPosition(float distance) const;

  Vector2 getDirection(float distance) const;

  Vector2 getAcceleration(float distance) const;

  float getLength() const;
};

}  // namespace WP_NAMESPACE
