#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <willpower/geometry/Edge.h>
#include <willpower/geometry/Mesh.h>
#include <willpower/geometry/MeshOperations.h>
#include <willpower/geometry/Polygon.h>
#include <willpower/geometry/UserAttributes.h>
#include <willpower/geometry/Vertex.h>

namespace {

using wp::Vector2;
using wp::geometry::DirectedEdgeVector;
using wp::geometry::Edge;
using wp::geometry::Mesh;
using wp::geometry::MeshCallbacks;
using wp::geometry::MeshOperations;
using wp::geometry::Polygon;
using wp::geometry::UserAttributes;
using wp::geometry::UserAttributesBase;
using wp::geometry::UserAttributesFactory;
using wp::geometry::Vertex;

void require(bool condition, char const* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct AttributeCounts {
  int created{};
  int copied{};
  int destroyed{};
};

class IntAttributes final : public UserAttributes<int> {
  AttributeCounts* mCounts;

public:
  explicit IntAttributes(AttributeCounts* counts)
      : mCounts(counts) {
    ++mCounts->created;
  }

  IntAttributes(IntAttributes const& other, AttributeCounts* counts)
      : UserAttributes<int>(other), mCounts(counts) {
    ++mCounts->copied;
  }

  ~IntAttributes() override {
    ++mCounts->destroyed;
  }

  uint32_t createAttribute(void const* data) override {
    return addAttribute(*static_cast<int const*>(data));
  }

  void updateAttribute(uint32_t index, void const* data) override {
    setAttribute(index, *static_cast<int const*>(data));
  }

  void const* readAttribute(uint32_t index) override {
    return &getAttribute(index);
  }
};

class IntAttributesFactory final : public UserAttributesFactory {
  AttributeCounts* mCounts;

public:
  explicit IntAttributesFactory(AttributeCounts* counts)
      : mCounts(counts) {
  }

  UserAttributesBase* create() override {
    return new IntAttributes(mCounts);
  }

  UserAttributesBase* copy(UserAttributesBase const* source) override {
    return new IntAttributes(*static_cast<IntAttributes const*>(source), mCounts);
  }
};

struct MeshFixture {
  uint32_t polygonIndex{};
  std::array<uint32_t, 4> vertexIndices{};
  std::array<uint32_t, 4> edgeIndices{};
};

MeshFixture populateSquare(Mesh& mesh, float offset) {
  MeshFixture fixture;
  fixture.vertexIndices = {
      mesh.addVertex(Vertex(offset + 0.0f, 0.0f)),
      mesh.addVertex(Vertex(offset + 2.0f, 0.0f)),
      mesh.addVertex(Vertex(offset + 2.0f, 2.0f)),
      mesh.addVertex(Vertex(offset + 0.0f, 2.0f))};
  fixture.edgeIndices = {
      mesh.addEdge(Edge(fixture.vertexIndices[0], fixture.vertexIndices[1])),
      mesh.addEdge(Edge(fixture.vertexIndices[1], fixture.vertexIndices[2])),
      mesh.addEdge(Edge(fixture.vertexIndices[2], fixture.vertexIndices[3])),
      mesh.addEdge(Edge(fixture.vertexIndices[3], fixture.vertexIndices[0]))};

  wp::geometry::IndexVector edgeData;
  for (size_t i = 0; i < fixture.edgeIndices.size(); ++i) {
    edgeData.push_back(fixture.vertexIndices[i]);
    edgeData.push_back(fixture.vertexIndices[(i + 1) % fixture.vertexIndices.size()]);
    edgeData.push_back(fixture.edgeIndices[i]);
  }
  fixture.polygonIndex = mesh.addPolygon(Polygon(edgeData));
  return fixture;
}

void setAttributes(Mesh& mesh, MeshFixture const& fixture, std::array<int, 4> const& values) {
  mesh.setVertexUserData(fixture.vertexIndices[0], &values[0]);
  mesh.setEdgeUserData(fixture.edgeIndices[0], &values[1]);
  mesh.setPolygonUserData(fixture.polygonIndex, &values[2]);
  mesh.setPolygonVertexUserData(fixture.polygonIndex, fixture.vertexIndices[0], &values[3]);
}

int readInt(void const* value) {
  require(value != nullptr, "assigned attribute must be present");
  return *static_cast<int const*>(value);
}

void populatedMeshAssignmentReleasesAndRebindsOwnedState() {
  std::array<AttributeCounts, 4> sourceCounts{};
  std::array<AttributeCounts, 4> destinationCounts{};
  std::array<IntAttributesFactory, 4> sourceFactories = {
      IntAttributesFactory(&sourceCounts[0]), IntAttributesFactory(&sourceCounts[1]),
      IntAttributesFactory(&sourceCounts[2]), IntAttributesFactory(&sourceCounts[3])};
  std::array<IntAttributesFactory, 4> destinationFactories = {
      IntAttributesFactory(&destinationCounts[0]), IntAttributesFactory(&destinationCounts[1]),
      IntAttributesFactory(&destinationCounts[2]), IntAttributesFactory(&destinationCounts[3])};

  Mesh source(&sourceFactories[0], &sourceFactories[1], &sourceFactories[2], &sourceFactories[3]);
  auto sourceFixture = populateSquare(source, 10.0f);
  std::array<int, 4> sourceValues{11, 22, 33, 44};
  setAttributes(source, sourceFixture, sourceValues);
  source.createAccelerationGrids(0.0f, -5.0f, 30.0f, 10.0f, 6, 2);

  int callbackCalls = 0;
  MeshCallbacks callbacks;
  callbacks.onAddVertex = [&callbackCalls](uint32_t, int32_t) { ++callbackCalls; };
  source.addMeshCallbacks(callbacks);
  Mesh* renderedMesh = nullptr;
  source.setRenderCallback([&renderedMesh](Mesh* mesh, void*) { renderedMesh = mesh; });

  Mesh destination(&destinationFactories[0], &destinationFactories[1],
                   &destinationFactories[2], &destinationFactories[3]);
  auto destinationFixture = populateSquare(destination, 0.0f);
  std::array<int, 4> destinationValues{1, 2, 3, 4};
  setAttributes(destination, destinationFixture, destinationValues);
  destination.createAccelerationGrids(-5.0f, -5.0f, 15.0f, 15.0f, 3, 3);

  destination = source;

  for (auto const& counts : destinationCounts) {
    require(counts.destroyed == 1,
            "assignment must destroy each replaced attribute collection exactly once");
  }
  for (auto const& counts : sourceCounts) {
    require(counts.destroyed == 0,
            "assignment must not destroy source or assigned attribute collections");
    require(counts.copied == 1, "assignment must copy each source attribute collection");
  }

  require(destination.getNumVertices() == 4, "assignment must preserve vertices");
  require(destination.getNumEdges() == 4, "assignment must preserve edges");
  require(destination.getNumPolygons() == 1, "assignment must preserve polygons");
  require(const_cast<Polygon&>(destination.getPolygon(sourceFixture.polygonIndex)).getMesh() ==
              &destination,
          "assigned polygons must refer to the destination mesh");
  require(destination.getPolygon(sourceFixture.polygonIndex).getEdges() ==
              source.getPolygon(sourceFixture.polygonIndex).getEdges(),
          "assignment must preserve directed polygon edges");
  require(destination.getPolygon(sourceFixture.polygonIndex).getOrderedVertexIndices() ==
              source.getPolygon(sourceFixture.polygonIndex).getOrderedVertexIndices(),
          "assignment must preserve polygon topology");

  require(readInt(destination.getVertexUserData(sourceFixture.vertexIndices[0])) == sourceValues[0],
          "assignment must preserve vertex attributes");
  require(readInt(destination.getEdgeUserData(sourceFixture.edgeIndices[0])) == sourceValues[1],
          "assignment must preserve edge attributes");
  require(readInt(destination.getPolygonUserData(sourceFixture.polygonIndex)) == sourceValues[2],
          "assignment must preserve polygon attributes");
  require(readInt(destination.getPolygonVertexUserData(
              sourceFixture.polygonIndex, sourceFixture.vertexIndices[0])) == sourceValues[3],
          "assignment must preserve polygon-vertex attributes");

  require(destination._getVertexAccelerationGrid() != source._getVertexAccelerationGrid(),
          "assigned vertex query grid must be independently owned");
  require(destination._getEdgeAccelerationGrid() != source._getEdgeAccelerationGrid(),
          "assigned edge query grid must be independently owned");
  require(destination._getPolygonAccelerationGrid() != source._getPolygonAccelerationGrid(),
          "assigned polygon query grid must be independently owned");
  require(destination.getContainingPolygon(Vector2(11.0f, 1.0f)) ==
              static_cast<int32_t>(sourceFixture.polygonIndex),
          "assigned polygon query grid must query destination geometry");

  destination.moveVertex(sourceFixture.vertexIndices[0], Vector2(1.0f, 0.0f));
  require(destination.getVertex(sourceFixture.vertexIndices[0]).getPosition() == Vector2(11.0f, 0.0f),
          "assigned vertex callbacks must update destination vertices");
  require(source.getVertex(sourceFixture.vertexIndices[0]).getPosition() == Vector2(10.0f, 0.0f),
          "assigned vertex callbacks must not update the source mesh");

  destination.addVertex(Vertex(20.0f, 0.0f));
  require(callbackCalls == 1, "assignment must preserve mesh callbacks");
  destination.renderCallback(nullptr);
  require(renderedMesh == &destination, "assigned render callback must receive the destination mesh");
}

void requireFiniteMeshVertices(Mesh const& mesh, std::vector<uint32_t> const& indices,
                               char const* message) {
  for (uint32_t index : indices) {
    auto const& position = mesh.getVertex(index).getPosition();
    require(std::isfinite(position.x) && std::isfinite(position.y), message);
  }
}

void shortCurveOperationsRetainInteriorGeometry() {
  Mesh chamferMesh;
  auto const chamferFixture = populateSquare(chamferMesh, 0.0f);
  wp::geometry::ChamferVertexResult chamferResult;
  MeshOperations::chamferVertex(&chamferMesh, chamferFixture.vertexIndices[0],
                                0.01f, MeshOperations::OptimalBezierCurvature,
                                &chamferResult);
  require(chamferResult.newVertexIndices.size() >= 3,
          "short chamfer collapsed its Bezier interior sample");
  requireFiniteMeshVertices(chamferMesh, chamferResult.newVertexIndices,
                            "short chamfer produced non-finite geometry");

  Mesh extrusionMesh;
  auto const extrusionFixture = populateSquare(extrusionMesh, 0.0f);
  wp::geometry::ExtrudeVertexResult extrusionResult;
  MeshOperations::extrudeVertex(
      &extrusionMesh, extrusionFixture.vertexIndices[0], 0.01f,
      wp::geometry::ExtrudeVertexOptions(wp::geometry::ExtrudeVertexOptions::Type::Round,
                                         true, 3.0f),
      &extrusionResult);
  require(extrusionResult.newVertexIndices.size() >= 3,
          "small round extrusion collapsed its interior arc sample");
  requireFiniteMeshVertices(extrusionMesh, extrusionResult.newVertexIndices,
                            "small round extrusion produced non-finite geometry");
}

void selfAssignmentPreservesMeshAndPolygonTopology() {
  Mesh mesh;
  auto fixture = populateSquare(mesh, 0.0f);
  mesh.createAccelerationGrids(-1.0f, -1.0f, 4.0f, 4.0f, 2, 2);

  auto expectedEdges = mesh.getPolygon(fixture.polygonIndex).getEdges();
  auto expectedVertices = mesh.getPolygon(fixture.polygonIndex).getOrderedVertexIndices();
  auto vertexGrid = mesh._getVertexAccelerationGrid();
  auto edgeGrid = mesh._getEdgeAccelerationGrid();
  auto polygonGrid = mesh._getPolygonAccelerationGrid();

  Mesh* meshSelf = &mesh;
  mesh = *meshSelf;

  require(mesh.getNumVertices() == 4 && mesh.getNumEdges() == 4 && mesh.getNumPolygons() == 1,
          "mesh self-assignment must preserve populated geometry");
  require(mesh.getPolygon(fixture.polygonIndex).getEdges() == expectedEdges,
          "mesh self-assignment must preserve directed edges");
  require(mesh.getPolygon(fixture.polygonIndex).getOrderedVertexIndices() == expectedVertices,
          "mesh self-assignment must preserve polygon topology");
  require(mesh._getVertexAccelerationGrid() == vertexGrid &&
              mesh._getEdgeAccelerationGrid() == edgeGrid &&
              mesh._getPolygonAccelerationGrid() == polygonGrid,
          "mesh self-assignment must be a no-op for query grids");

  Polygon polygon = mesh.getPolygon(fixture.polygonIndex);
  auto polygonEdges = polygon.getEdges();
  auto polygonVertices = polygon.getVertexIndexList();
  Polygon* polygonSelf = &polygon;
  polygon = *polygonSelf;
  require(polygon.getEdges() == polygonEdges,
          "polygon self-assignment must preserve directed edges");
  require(polygon.getVertexIndexList() == polygonVertices,
          "polygon self-assignment must preserve topology");
}

}  // namespace

int main() {
  try {
    populatedMeshAssignmentReleasesAndRebindsOwnedState();
    shortCurveOperationsRetainInteriorGeometry();
    selfAssignmentPreservesMeshAndPolygonTopology();
    std::cout << "Geometry mesh assignment tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
