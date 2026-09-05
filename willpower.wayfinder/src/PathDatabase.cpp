#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "willpower/wayfinder/ConvexPolygonisation.h"
#include "willpower/wayfinder/PathDatabase.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;

		void PathDatabase::setSize(uint32_t numPolygons)
		{
			constexpr uint32_t polygonCapacity =
				static_cast<uint32_t>(numeric_limits<PolygonIndex>::max()) + 1;
			if (numPolygons == 0 || numPolygons > polygonCapacity)
			{
				throw invalid_argument("PathDatabase polygon count is outside its 16-bit index capacity.");
			}

			mRoutesByTarget.clear();
			mUseCounter = 0;
			mNumPolygons = numPolygons;
		}

		void PathDatabase::setMaxCachedTargets(size_t maxCachedTargets)
		{
			if (maxCachedTargets == 0)
			{
				throw invalid_argument("PathDatabase must cache at least one target.");
			}

			mMaxCachedTargets = maxCachedTargets;
			while (mRoutesByTarget.size() > mMaxCachedTargets)
			{
				evictOldestTarget();
			}
		}

		size_t PathDatabase::getMaxCachedTargets() const
		{
			return mMaxCachedTargets;
		}

		size_t PathDatabase::getNumCachedTargets() const
		{
			return mRoutesByTarget.size();
		}

		void PathDatabase::touch(TargetRoutes& routes) const
		{
			routes.lastUsed = ++mUseCounter;
		}

		void PathDatabase::evictOldestTarget()
		{
			auto oldest = min_element(
				mRoutesByTarget.begin(), mRoutesByTarget.end(),
				[](auto const& lhs, auto const& rhs)
				{
					return lhs.second.lastUsed < rhs.second.lastUsed;
				});
			if (oldest != mRoutesByTarget.end())
			{
				mRoutesByTarget.erase(oldest);
			}
		}

		PathDatabase::TargetRoutes& PathDatabase::resetTarget(PolygonIndex target)
		{
			if (target >= mNumPolygons)
			{
				throw out_of_range("PathDatabase target polygon is out of range.");
			}

			auto existing = mRoutesByTarget.find(target);
			if (existing != mRoutesByTarget.end())
			{
				fill(existing->second.nextNeighbour.begin(),
					existing->second.nextNeighbour.end(), NoRoute);
				existing->second.calculated = false;
				touch(existing->second);
				return existing->second;
			}

			TargetRoutes routes;
			routes.nextNeighbour.assign(mNumPolygons, NoRoute);
			touch(routes);
			auto result = mRoutesByTarget.emplace(target, move(routes));
			if (mRoutesByTarget.size() > mMaxCachedTargets)
			{
				evictOldestTarget();
			}
			return result.first->second;
		}

		void PathDatabase::setNextNeighbour(
			PolygonIndex target, PolygonIndex source, uint8_t nextNeighbour)
		{
			auto routes = mRoutesByTarget.find(target);
			if (source >= mNumPolygons || routes == mRoutesByTarget.end())
			{
				throw out_of_range("PathDatabase route polygon is out of range.");
			}
			routes->second.nextNeighbour[source] = nextNeighbour;
		}

		vector<EdgeIndex> PathDatabase::getPath(
			PolygonIndex source, PolygonIndex target,
			ConvexPolygonisation const& polygons) const
		{
			if (source >= mNumPolygons || target >= mNumPolygons)
			{
				throw out_of_range("PathDatabase path polygon is out of range.");
			}
			if (source == target)
			{
				return {};
			}

			auto routes = mRoutesByTarget.find(target);
			if (routes == mRoutesByTarget.end() || !routes->second.calculated)
			{
				throw logic_error("PathDatabase target has not been calculated.");
			}
			touch(routes->second);

			vector<EdgeIndex> path;
			auto current = source;
			while (current != target)
			{
				auto neighbourIndex = routes->second.nextNeighbour[current];
				auto const& polygon = polygons.mPolygons[current];
				if (neighbourIndex == NoRoute)
				{
					return {};
				}
				if (neighbourIndex >= polygon.numNeighbours)
				{
					throw logic_error("PathDatabase target tree contains an invalid neighbour.");
				}

				auto next = polygon.neighbours[neighbourIndex];
				auto key = (static_cast<uint32_t>(current) << 16) + next;
				path.push_back(polygons.mPolygonToEdgeLookup.at(key));
				current = next;
				if (path.size() >= mNumPolygons)
				{
					throw logic_error("PathDatabase target tree contains a cycle.");
				}
			}
			return path;
		}

		void PathDatabase::setCalculated(PolygonIndex target)
		{
			auto routes = mRoutesByTarget.find(target);
			if (routes == mRoutesByTarget.end())
			{
				throw out_of_range("PathDatabase target polygon is out of range.");
			}
			routes->second.calculated = true;
			touch(routes->second);
		}

		bool PathDatabase::isCalculated(PolygonIndex target) const
		{
			auto routes = mRoutesByTarget.find(target);
			if (routes == mRoutesByTarget.end() || !routes->second.calculated)
			{
				return false;
			}
			touch(routes->second);
			return true;
		}

	} // wayfinder
} // WP_NAMESPACE
