#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <willpower/common/BezierSpline.h>

namespace {

using wp::BezierSpline;
using wp::BoundingBox;
using wp::SplinePath;
using wp::Vector2;

void require(bool condition, std::string const& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireFinite(Vector2 const& point, std::string const& message) {
  require(std::isfinite(point.x) && std::isfinite(point.y), message);
}

void requireTessellation(std::vector<Vector2> const& samples, Vector2 const& start,
                         Vector2 const& end, std::string const& name) {
  require(samples.size() >= static_cast<size_t>(SplinePath::MinimumTessellationSampleCount),
          name + " did not include the documented minimum sample count");
  require(samples.front() == start && samples.back() == end,
          name + " did not preserve both endpoints");
  for (Vector2 const& sample : samples) {
    requireFinite(sample, name + " produced a non-finite sample");
  }
}

class LinearSpline final : public SplinePath {
  float mLength;

public:
  explicit LinearSpline(float length)
      : SplinePath({{0.0f, 0.0f}, {0.0f, 0.0f}, {length, 0.0f}, {length, 0.0f}}),
        mLength(length) {
  }

  Vector2 getPosition(float distance) const override {
    return {distance, 0.0f};
  }

  Vector2 getDirection(float) const override {
    return {1.0f, 0.0f};
  }

  Vector2 getAcceleration(float) const override {
    return {0.0f, 0.0f};
  }

  float getLength() const override {
    return mLength;
  }

  BoundingBox getBounds() const override {
    return {{0.0f, 0.0f}, {mLength, 0.0f}};
  }
};

void shortSplinePathAvoidsZeroAndOneSampleDivision() {
  for (float length : {0.25f, 1.0f}) {
    LinearSpline spline(length);
    auto const samples = spline.divide();
    requireTessellation(samples, {0.0f, 0.0f}, {length, 0.0f},
                        "short linear spline");
  }
}

void shortBezierCurveHasEndpointsAndInteriorSample() {
  std::vector<Vector2> const controls{
      {0.0f, 0.0f}, {0.0f, 0.25f}, {0.25f, 0.25f}, {0.25f, 0.0f}};
  BezierSpline curve(controls);

  auto const samples = curve.divide();
  requireTessellation(samples, controls.front(), controls.back(), "short Bezier curve");
}

}  // namespace

int main() {
  try {
    shortSplinePathAvoidsZeroAndOneSampleDivision();
    shortBezierCurveHasEndpointsAndInteriorSample();
    std::cout << "Curve tessellation retains a finite minimum sample set\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
