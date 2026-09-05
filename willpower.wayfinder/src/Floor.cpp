#include <utils/StringUtils.h>

#include <stdexcept>

#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/geometry/MeshUtils.h"

#include "willpower/wayfinder/Floor.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		Floor::Floor()
			: mInset(0)
			, mPolygons(nullptr)
			, mPathDatabase(nullptr)
		{
		}

		Floor::Floor(vector<Vector2> const& border)
			: Floor()
		{
			mBorder = border;
		}

		Floor::Floor(vector<Vector2> const& border, vector<vector<Vector2>> const& holes)
			: Floor()
		{
			mBorder = border;
			mHoles = holes;
		}

		Floor::Floor(
			vector<Vector2> const& border, vector<vector<Vector2>> const& holes,
			vector<Vector2> const& vertices, vector<Triangle> const& triangles)
			: Floor(border, holes)
		{
			setTriangulation(vertices, triangles);
		}

		Floor::~Floor()
		{
			delete mPolygons;
			delete mPathDatabase;
		}

		void Floor::setBorder(vector<Vector2> const& border)
		{
			mBorder = border;
		}

		void Floor::addHole(vector<Vector2> const& hole)
		{
			mHoles.push_back(hole);
		}

		void Floor::addHoles(vector<vector<Vector2>> const& holes)
		{
			mHoles = holes;
		}

		vector<Vector2> const& Floor::getBorder() const
		{
			return mBorder;
		}

		uint32_t Floor::getNumHoles() const
		{
			return (uint32_t)mHoles.size();
		}

		vector<Vector2> const& Floor::getHole(uint32_t index) const
		{
			ASSERT_TRACE(index < mHoles.size() && "Sector::getHole(): index is out of range.");
			return mHoles[index];
		}

		void Floor::triangulate(int inset)
		{
			mInset = inset;

			vector<vector<Vector2>> inLoops = mHoles;
			inLoops.push_back(mBorder);

			auto outLoops = geometry::MeshUtils::insetVertexLoops(inLoops, (float)inset, false);

			std::vector<IsolatedArea> areas;
			for (auto loop: outLoops)
			{
				if (geometry::MeshUtils::getVertexWinding(loop) == Winding::Anticlockwise)
				{
					// Add to border list
					IsolatedArea area;

					area.border = loop;
					areas.push_back(area);
				}
				else
				{
					// Check which area to assign to.
					for (auto& area: areas)
					{
						if (MathsUtils::pointInPolygon(loop.front(), area.border))
						{
							area.holes.push_back(loop);
							break;
						}
					}
				}
			}

			// Triangulate
			delete mPolygons;
			mPolygons = new ConvexPolygonisation();

			for (auto const& area: areas)
			{
				mPolygons->addArea(area.border, area.holes);
			}

			initialiseNavigationData();
		}

		void Floor::setTriangulation(
			vector<Vector2> const& vertices, vector<Triangle> const& triangles)
		{
			mInset = 0;
			delete mPolygons;
			mPolygons = new ConvexPolygonisation();
			mPolygons->addTriangulation(vertices, triangles);
			initialiseNavigationData();
		}

		void Floor::initialiseNavigationData()
		{
			// Build acceleration grid
			mPolygons->createAccelerationGrids(10, 10);

			// Pathfinding (PortalGraph)
			mPolygons->buildPathGraph();

			// Pathfinding (PathDatabase)
			delete mPathDatabase;
			mPathDatabase = new PathDatabase();
			mPathDatabase->setMaxCachedTargets(mMaxCachedPathTargets);
			mPathDatabase->setSize(mPolygons->getNumPolygons());
		}

		void Floor::composeConvex(int maxDegree)
		{
			if (mPolygons)
			{
				mPolygons->composeConvex(maxDegree);
				mPolygons->createAccelerationGrids(10, 10);
				mPolygons->buildPathGraph();
			}
		}

		void Floor::calculatePaths(PolygonIndex target, bool recalculate) const
		{
			if (recalculate || !mPathDatabase->isCalculated(target))
			{
				mPolygons->calculatePaths(target, mPathDatabase);
			}
		}

		void Floor::calculatePaths(PolygonIndex target)
		{
			calculatePaths(target, false);
		}

		void Floor::recalculatePaths(PolygonIndex target)
		{
			calculatePaths(target, true);
		}

		void Floor::setMaxCachedPathTargets(size_t maxCachedTargets)
		{
			if (maxCachedTargets == 0)
			{
				throw invalid_argument("Floor must cache at least one path target.");
			}
			if (mPathDatabase)
			{
				mPathDatabase->setMaxCachedTargets(maxCachedTargets);
			}
			mMaxCachedPathTargets = maxCachedTargets;
		}

		size_t Floor::getMaxCachedPathTargets() const
		{
			return mMaxCachedPathTargets;
		}

		size_t Floor::getNumCachedPathTargets() const
		{
			return mPathDatabase ? mPathDatabase->getNumCachedTargets() : 0;
		}

		vector<int> Floor::calculatePathLengths(PolygonIndex target) const
		{
			return mPolygons->calculatePathLengths(target);
		}

		AgentPath Floor::getPath(Vector2 const& position, PolygonIndex source, PolygonIndex target, float startTimer) const
		{
			return AgentPath(
				position, mPathDatabase->getPath(source, target, *mPolygons),
				mPolygons, startTimer);
		}

		int Floor::getInset() const
		{
			return mInset;
		}

		ConvexPolygonisation const* Floor::getPolygonisation() const
		{
			return mPolygons;
		}

		uint32_t Floor::getNumPolygons() const
		{
			return mPolygons->getNumPolygons();
		}

		int32_t Floor::getContainingPolygon(float x, float y) const
		{
			return mPolygons->getContainingPolygon(x, y);
		}

		int32_t Floor::getContainingPolygon(Vector2 const& position) const
		{
			return mPolygons->getContainingPolygon(position);
		}
	} // wayfinder
} // WP_NAMESPACE
