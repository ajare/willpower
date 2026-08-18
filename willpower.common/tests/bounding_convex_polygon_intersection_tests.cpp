#include <iostream>
#include <stdexcept>
#include <vector>

#include <willpower/common/BoundingConvexPolygon.h>
#include <willpower/common/Triangulation.h>
#include <willpower/common/Vector2.h>

namespace {

void require(bool condition, char const* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

wp::Triangulation intersectingTriangleMesh() {
  wp::Triangulation mesh;
  mesh.addTriangle({0.0f, 0.0f}, {2.0f, 0.0f}, {0.0f, 2.0f});
  return mesh;
}

void shortPolygonsDoNotIntersectTriangleMeshes() {
  std::vector<wp::Vector2> const vertices{{0.0f, 0.0f}, {2.0f, 0.0f}, {0.0f, 2.0f}};
  auto const mesh = intersectingTriangleMesh();

  for (std::size_t vertexCount = 0; vertexCount < vertices.size(); ++vertexCount) {
    wp::BoundingConvexPolygon polygon(
        wp::Vector2::ZERO,
        std::vector<wp::Vector2>(vertices.begin(), vertices.begin() + vertexCount));

    require(!polygon.intersectsTriMesh(mesh),
            "a polygon with fewer than three vertices intersected a triangle mesh");
  }

  wp::BoundingConvexPolygon triangle(wp::Vector2::ZERO, vertices);
  require(triangle.intersectsTriMesh(mesh),
          "a three-vertex polygon did not intersect an overlapping triangle mesh");
}

}  // namespace

int main() {
  try {
    shortPolygonsDoNotIntersectTriangleMeshes();
    std::cout << "Bounding convex polygon intersection regressions passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
