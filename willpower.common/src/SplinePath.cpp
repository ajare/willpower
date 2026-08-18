#include <algorithm>

#include "willpower/common/SplinePath.h"

using namespace std;

namespace WP_NAMESPACE {

SplinePath::SplinePath() {
}

SplinePath::SplinePath(vector<Vector2> const& points)
    : mPoints(points) {
  int numPoints = (int)points.size();
  if (numPoints < 4) {
    throw exception("SplinePath: cubic curves require at least 4 points.");
  }
}

int SplinePath::getNumControlPoints() const {
  return (int)mPoints.size();
}

Vector2 const& SplinePath::getControlPoint(int index) const {
  return mPoints[index];
}

void SplinePath::setControlPoint(int index, Vector2 const& position) {
  mPoints[index] = position;
  rebuildRenderable();
}

vector<Vector2> SplinePath::divide(float scale) const {
  vector<Vector2> vertices;

  float const length = getLength();
  int const sampleCount = max(MinimumTessellationSampleCount,
                              static_cast<int>(length * scale));
  float const dt = 1.0f / static_cast<float>(sampleCount - 1);

  for (int i = 0; i < sampleCount; ++i) {
    vertices.push_back(getPosition(i * dt * length));
  }

  return vertices;
}

BoundingBox SplinePath::getBounds() const {
  // Piecemeal approximate.
  BoundingBox bounds(divide());

  // Expand a little to allow for inaccuracies in the curve approximation
  bounds.expand(0.01f, 0.01f);

  return bounds;
}

}  // namespace WP_NAMESPACE
