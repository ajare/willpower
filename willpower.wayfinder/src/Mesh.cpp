#include <utils/StringUtils.h>

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
