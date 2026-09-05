#pragma once

#include "willpower/common/Vector2.h"
#include "willpower/common/MathsUtils.h"
#include "willpower/common/AccelerationGrid.h"
#include "willpower/common/Logger.h"

#include "willpower/geometry/Mesh.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Sector.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the mesh type.
		 */
		class WP_WAYFINDER_API Mesh
		{
			std::vector<Sector*> mSectors;

			BoundingBox mBounds;

		private:

			/**
			 * @brief Creates sectors.
			 * @param geometryMesh The geometryMesh parameter used by the method.
			 */
			void createSectors(geometry::Mesh const* geometryMesh);

			/**
			 * @brief Sets the bounds.
			 * @param geometryMesh The geometryMesh parameter used by the method.
			 */
			void setBounds(geometry::Mesh const* geometryMesh);

		public:

			/**
			 * @brief Performs the mesh operation.
			 * @param geometryMesh The geometryMesh parameter used by the method.
			 * @param insetSize The insetSize parameter used by the method.
			 */
			Mesh(geometry::Mesh const* geometryMesh, int insetSize);

			/**
			 * @brief Performs the mesh operation.
			 * @param geometryMesh The geometryMesh parameter used by the method.
			 * @param insetSizes The insetSizes parameter used by the method.
			 */
			Mesh(geometry::Mesh const* geometryMesh, std::vector<int> const& insetSizes);

			/**
			 * @brief Destroys the mesh instance.
			 */
			~Mesh();

			/**
			 * @brief Performs the compose convex operation.
			 * @param maxDegree The maxDegree parameter used by the method.
			 */
			void composeConvex(int maxDegree = ConvexPolygonisation::MaxPolygonDegree);

			/**
			 * @brief Gets the num sectors.
			 * @return The requested value or operation result.
			 */
			uint32_t getNumSectors() const;

			/**
			 * @brief Gets the sector.
			 * @param index Zero-based index to read or write.
			 * @return The requested value or operation result.
			 */
			Sector* getSector(uint32_t index);

			/**
			 * @brief Gets the containing sector.
			 * @param insetSize The insetSize parameter used by the method.
			 * @param x X coordinate or component value.
			 * @param y Y coordinate or component value.
			 * @return The requested value or operation result.
			 */
			Sector* getContainingSector(int insetSize, float x, float y);

			/**
			 * @brief Gets the containing sector.
			 * @param insetSize The insetSize parameter used by the method.
			 * @param position Position value used by the method.
			 * @return The requested value or operation result.
			 */
			Sector* getContainingSector(int insetSize, Vector2 const& position);

			/**
			 * @brief Gets the bounds.
			 * @return The requested value or operation result.
			 */
			BoundingBox const& getBounds() const;
		};

	} // wayfinder
} // WP_NAMESPACE
