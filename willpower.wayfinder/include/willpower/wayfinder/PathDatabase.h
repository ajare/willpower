#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Types.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		class ConvexPolygonisation;

		/**
		 * @brief Caches compact reverse shortest-path trees for recently used targets.
		 */
		class WP_WAYFINDER_API PathDatabase
		{
			friend class ConvexPolygonisation;

			static constexpr uint8_t NoRoute = 0xff;

			struct TargetRoutes
			{
				std::vector<uint8_t> nextNeighbour;
				uint64_t lastUsed{0};
				bool calculated{false};
			};

			uint32_t mNumPolygons{0};
			size_t mMaxCachedTargets{32};
			mutable uint64_t mUseCounter{0};
			mutable std::unordered_map<PolygonIndex, TargetRoutes> mRoutesByTarget;

			TargetRoutes& resetTarget(PolygonIndex target);

			void setNextNeighbour(
				PolygonIndex target,
				PolygonIndex source,
				uint8_t nextNeighbour);

			void touch(TargetRoutes& routes) const;

			void evictOldestTarget();

		public:

			PathDatabase() = default;

			PathDatabase(PathDatabase const&) = delete;

			PathDatabase& operator=(PathDatabase const&) = delete;

			PathDatabase(PathDatabase&&) noexcept = default;

			PathDatabase& operator=(PathDatabase&&) noexcept = default;

			~PathDatabase() = default;

			/**
			 * @brief Resets the cache for a polygon count without allocating routes.
			 * @param numPolygons Number of polygons in the path graph.
			 */
			void setSize(uint32_t numPolygons);

			/**
			 * @brief Sets the maximum number of target trees retained by the cache.
			 */
			void setMaxCachedTargets(size_t maxCachedTargets);

			size_t getMaxCachedTargets() const;

			size_t getNumCachedTargets() const;

			/**
			 * @brief Reconstructs the portal sequence from source to target.
			 */
			std::vector<EdgeIndex> getPath(
				PolygonIndex source,
				PolygonIndex target,
				ConvexPolygonisation const& polygons) const;

			void setCalculated(PolygonIndex target);

			bool isCalculated(PolygonIndex target) const;
		};

	} // wayfinder
} // WP_NAMESPACE
