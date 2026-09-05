#pragma once

#include "willpower/common/Vector2.h"
#include "willpower/common/MathsUtils.h"
#include "willpower/common/AccelerationGrid.h"
#include "willpower/common/Logger.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Floor.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the sector type.
		 */
		class WP_WAYFINDER_API Sector
		{
			int32_t mId;

			std::vector<Vector2> mBorder;

			std::vector<std::vector<Vector2>> mHoles;

			BoundingBox mBounds;

			std::map<int, Floor*> mFloors;

			// Edge lookup
			std::vector<Vector2> mEdgeList;

			AccelerationGrid* mEdgesGrid;

		private:

			/**
			 * @brief Updates bounds.
			 * @param points The points parameter used by the method.
			 */
			void updateBounds(std::vector<Vector2> const& points);

		public:

			/**
			 * @brief Performs the sector operation.
			 */
			Sector();

			/**
			 * @brief Performs the sector operation.
			 * @param border The border parameter used by the method.
			 */
			explicit Sector(std::vector<Vector2> const& border);

			/**
			 * @brief Performs the sector operation.
			 * @param border The border parameter used by the method.
			 * @param holes The holes parameter used by the method.
			 */
			Sector(std::vector<Vector2> const& border, std::vector<std::vector<Vector2>> const& holes);

			/**
			 * @brief Destroys the sector instance.
			 */
			~Sector();

			/**
			 * @brief Sets the id.
			 * @param id The id parameter used by the method.
			 */
			void setId(int32_t id);

			/**
			 * @brief Gets the id.
			 * @return The requested value or operation result.
			 */
			int32_t getId() const;

			/**
			 * @brief Sets the border.
			 * @param border The border parameter used by the method.
			 */
			void setBorder(std::vector<Vector2> const& border);

			/**
			 * @brief Adds hole.
			 * @param hole The hole parameter used by the method.
			 */
			void addHole(std::vector<Vector2> const& hole);

			/**
			 * @brief Adds holes.
			 * @param holes The holes parameter used by the method.
			 */
			void addHoles(std::vector<std::vector<Vector2>> const& holes);

			/**
			 * @brief Gets the border.
			 * @return The requested value or operation result.
			 */
			std::vector<Vector2> const& getBorder() const;

			/**
			 * @brief Gets the num holes.
			 * @return The requested value or operation result.
			 */
			uint32_t getNumHoles() const;

			/**
			 * @brief Gets the hole.
			 * @param index Zero-based index to read or write.
			 * @return The requested value or operation result.
			 */
			std::vector<Vector2> const& getHole(uint32_t index) const;

			/**
			 * @brief Gets the edge vertices within radius.
			 * @param position Position value used by the method.
			 * @param radius Radius value used by the method.
			 * @return The requested value or operation result.
			 */
			std::vector<Vector2> getEdgeVerticesWithinRadius(Vector2 const& position, float radius) const;

			/**
			 * @brief Creates acceleration grid.
			 */
			void createAccelerationGrid();

			/**
			 * @brief Creates floor.
			 * @param insetSize The insetSize parameter used by the method.
			 * @return The requested value or operation result.
			 */
			Floor* createFloor(int insetSize);

			/**
			 * @brief Gets the floor inset sizes.
			 * @return The requested value or operation result.
			 */
			std::vector<int> getFloorInsetSizes() const;

			/**
			 * @brief Determines whether the object has floor.
			 * @param insetSize The insetSize parameter used by the method.
			 * @return The requested value or operation result.
			 */
			bool hasFloor(int insetSize) const;

			/**
			 * @brief Gets the floor.
			 * @param insetSize The insetSize parameter used by the method.
			 * @return The requested value or operation result.
			 */
			Floor* getFloor(int insetSize);

			/**
			 * @brief Performs the contains point operation.
			 * @param insetSize The insetSize parameter used by the method.
			 * @param x X coordinate or component value.
			 * @param y Y coordinate or component value.
			 * @return The requested value or operation result.
			 */
			bool containsPoint(int insetSize, float x, float y) const;

			/**
			 * @brief Performs the compose convex operation.
			 * @param maxDegree The maxDegree parameter used by the method.
			 */
			void composeConvex(int maxDegree = ConvexPolygonisation::MaxPolygonDegree);
		};

	} // wayfinder
} // WP_NAMESPACE
