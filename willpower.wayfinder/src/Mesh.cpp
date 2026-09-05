#include <utils/StringUtils.h>

#include <algorithm>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>

#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/geometry/MeshUtils.h"

#include "willpower/wayfinder/Mesh.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		namespace
		{
			struct TriangleEdgeUse
			{
				uint32_t triangle;
				uint32_t v0;
				uint32_t v1;
			};

			class TriangleComponents
			{
				vector<uint32_t> mParents;

			public:
				explicit TriangleComponents(size_t size) : mParents(size)
				{
					iota(mParents.begin(), mParents.end(), 0);
				}

				uint32_t find(uint32_t index)
				{
					while (mParents[index] != index)
					{
						mParents[index] = mParents[mParents[index]];
						index = mParents[index];
					}
					return index;
				}

				void join(uint32_t first, uint32_t second)
				{
					first = find(first);
					second = find(second);
					if (first != second)
					{
						mParents[second] = first;
					}
				}
			};
		}

		Mesh::Mesh(geometry::Mesh const* geometryMesh, int insetSize)
		{
			createSectors(geometryMesh);

			for (auto& sector: mSectors)
			{
				sector->createFloor(insetSize);
			}

			setBounds(geometryMesh);
		}

		Mesh::Mesh(geometry::Mesh const* geometryMesh, vector<int> const& insetSizes)
		{
			createSectors(geometryMesh);

			for (auto& sector: mSectors)
			{
				for (int insetSize: insetSizes)
				{
					sector->createFloor(insetSize);
				}
			}

			setBounds(geometryMesh);
		}

		Mesh::Mesh(vector<Vector2> const& vertices, vector<Triangle> const& triangles)
		{
			if (triangles.empty())
			{
				mBounds.setPosition(Vector2(0, 0));
				mBounds.setSize(Vector2(0, 0));
				return;
			}

			vector<Triangle> orderedTriangles = triangles;
			TriangleComponents components(triangles.size());
			map<pair<uint32_t, uint32_t>, vector<TriangleEdgeUse>> edgeUses;
			for (uint32_t triangleIndex = 0; triangleIndex < orderedTriangles.size(); ++triangleIndex)
			{
				auto& triangle = orderedTriangles[triangleIndex];
				for (auto vertex: triangle)
				{
					if (vertex >= vertices.size())
					{
						throw out_of_range("Wayfinder triangle vertex index is out of range.");
					}
				}
				if (MathsUtils::pointSideOnLine(
					vertices[triangle[0]], vertices[triangle[1]],
					vertices[triangle[2]]) == MathsUtils::Side::Right)
				{
					swap(triangle[0], triangle[1]);
				}

				for (size_t i = 0; i < 3; ++i)
				{
					auto v0 = triangle[i];
					auto v1 = triangle[(i + 1) % 3];
					auto key = minmax(v0, v1);
					auto& uses = edgeUses[{key.first, key.second}];
					if (!uses.empty())
					{
						components.join(triangleIndex, uses.front().triangle);
					}
					uses.push_back({triangleIndex, v0, v1});
				}
			}

			map<uint32_t, vector<uint32_t>> componentTriangles;
			for (uint32_t i = 0; i < orderedTriangles.size(); ++i)
			{
				componentTriangles[components.find(i)].push_back(i);
			}
			map<uint32_t, vector<TriangleEdgeUse>> componentBoundaryEdges;
			for (auto const& edgeUseEntry: edgeUses)
			{
				auto const& uses = edgeUseEntry.second;
				if (uses.size() == 1)
				{
					componentBoundaryEdges[components.find(uses.front().triangle)].push_back(uses.front());
				}
			}

			bool haveBounds = false;
			Vector2 minExtent, maxExtent;
			for (auto const& [root, triangleIndices]: componentTriangles)
			{
				auto sector = make_unique<Sector>();
				auto const& boundaryEdges = componentBoundaryEdges[root];

				multimap<uint32_t, size_t> outgoingEdges;
				for (size_t i = 0; i < boundaryEdges.size(); ++i)
				{
					outgoingEdges.emplace(boundaryEdges[i].v0, i);
				}
				vector<bool> usedBoundaryEdges(boundaryEdges.size(), false);
				for (size_t firstEdge = 0; firstEdge < boundaryEdges.size(); ++firstEdge)
				{
					if (usedBoundaryEdges[firstEdge])
					{
						continue;
					}
					vector<Vector2> loop;
					auto edgeIndex = firstEdge;
					auto firstVertex = boundaryEdges[firstEdge].v0;
					while (!usedBoundaryEdges[edgeIndex])
					{
						auto const& edge = boundaryEdges[edgeIndex];
						usedBoundaryEdges[edgeIndex] = true;
						loop.push_back(vertices[edge.v0]);
						if (edge.v1 == firstVertex)
						{
							break;
						}
						auto range = outgoingEdges.equal_range(edge.v1);
						auto next = find_if(range.first, range.second, [&](auto const& candidate)
						{
							return !usedBoundaryEdges[candidate.second];
						});
						if (next == range.second)
						{
							throw invalid_argument("Wayfinder triangulation has an open boundary.");
						}
						edgeIndex = next->second;
					}

					if (geometry::MeshUtils::getVertexWinding(loop) == Winding::Anticlockwise)
					{
						sector->setBorder(loop);
					}
					else
					{
						sector->addHole(loop);
					}
				}

				map<uint32_t, uint32_t> vertexRemapping;
				vector<Vector2> localVertices;
				vector<Triangle> localTriangles;
				localTriangles.reserve(triangleIndices.size());
				for (auto triangleIndex: triangleIndices)
				{
					Triangle localTriangle;
					for (size_t i = 0; i < 3; ++i)
					{
						auto sourceIndex = orderedTriangles[triangleIndex][i];
						auto [found, inserted] = vertexRemapping.try_emplace(
							sourceIndex, static_cast<uint32_t>(localVertices.size()));
						if (inserted)
						{
							auto const& vertex = vertices[sourceIndex];
							localVertices.push_back(vertex);
							if (!haveBounds)
							{
								minExtent = maxExtent = vertex;
								haveBounds = true;
							}
							else
							{
								minExtent.x = min(minExtent.x, vertex.x);
								minExtent.y = min(minExtent.y, vertex.y);
								maxExtent.x = max(maxExtent.x, vertex.x);
								maxExtent.y = max(maxExtent.y, vertex.y);
							}
						}
						localTriangle[i] = found->second;
					}
					localTriangles.push_back(localTriangle);
				}

				sector->setId(static_cast<int32_t>(mSectors.size()));
				sector->createAccelerationGrid();
				sector->createFloor(localVertices, localTriangles);
				mSectors.push_back(sector.release());
			}

			mBounds.setPosition(minExtent);
			mBounds.setSize(maxExtent - minExtent);
		}

		Mesh::~Mesh()
		{
			for (auto sector: mSectors)
			{
				delete sector;
			}
		}

		void Mesh::createSectors(geometry::Mesh const* geometryMesh)
		{
			auto groups = geometry::MeshUtils::groupConnectedPolygons(geometryMesh);

			for (auto const& group: groups)
			{
				Sector* sector = new Sector();

				list<geometry::MeshUtils::EdgeIndexInfo> edges;

				for (auto polygonId : group)
				{
					// Get all edges, and if they have one polygon,
					// then add to set to be joined together.
					auto const& polygon = geometryMesh->getPolygon(polygonId);
					auto const& edgeIndices = polygon.getEdgeIndexSet();

					for (auto edgeIndex : edgeIndices)
					{
						auto const& edge = geometryMesh->getEdge(edgeIndex);
						if (edge.getPolygonReferences().size() == 1)
						{
							auto edgeIt = polygon.getEdgeByIndex(edgeIndex);
							edges.push_back(make_tuple((uint32_t)edgeIndex, edgeIt->v0, edgeIt->v1));
						}
					}
				}

				// Domino sort them.
				vector<vector<geometry::MeshUtils::EdgeIndexInfo>> edgeGroups
					= geometry::MeshUtils::groupConnectedEdges(edges);

				// Extract vertices
				for (auto const& edgeGroup : edgeGroups)
				{
					vector<Vector2> vertices;

					for (auto const& vertex : edgeGroup)
					{
						uint32_t vertexIndex = get<1>(vertex);
						Vector2 vertexPos = geometryMesh->getVertex(vertexIndex).getPosition();

						vertices.push_back(vertexPos);
					}

					// Is this edge group a border or a hole?
					if (geometry::MeshUtils::getVertexWinding(vertices) == Winding::Anticlockwise)
					{
						sector->setBorder(vertices);
					}
					else
					{
						sector->addHole(vertices);
					}
				}

				sector->setId((int32_t)mSectors.size());
				sector->createAccelerationGrid();
				mSectors.push_back(sector);
			}
		}

		void Mesh::setBounds(geometry::Mesh const* geometryMesh)
		{
			Vector2 minExtent, maxExtent;
			geometryMesh->getExtents(minExtent, maxExtent);

			mBounds.setPosition(minExtent);
			mBounds.setSize(maxExtent - minExtent);
		}

		void Mesh::composeConvex(int maxDegree)
		{
			for (auto sector: mSectors)
			{
				sector->composeConvex(maxDegree);
			}
		}

		uint32_t Mesh::getNumSectors() const
		{
			return (uint32_t)mSectors.size();
		}

		Sector* Mesh::getSector(uint32_t index)
		{
			ASSERT_TRACE(index < mSectors.size() && "Mesh::getSector(): index is out of range.");
			return mSectors[index];
		}

		Sector* Mesh::getContainingSector(int insetSize, float x, float y)
		{
			for (auto const& sector: mSectors)
			{
				if (sector->containsPoint(insetSize, x, y))
				{
					return sector;
				}
			}

			return nullptr;
		}

		Sector* Mesh::getContainingSector(int insetSize, Vector2 const& position)
		{
			return getContainingSector(insetSize, position.x, position.y);
		}

		BoundingBox const& Mesh::getBounds() const
		{
			return mBounds;
		}

	} // wayfinder
} // WP_NAMESPACE
