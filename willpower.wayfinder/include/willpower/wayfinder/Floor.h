#pragma once

#include "willpower/common/Vector2.h"
#include "willpower/common/MathsUtils.h"
#include "willpower/common/AccelerationGrid.h"
#include "willpower/common/Logger.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Types.h"
#include "willpower/wayfinder/ConvexPolygonisation.h"
#include "willpower/wayfinder/PathDatabase.h"
#include "willpower/wayfinder/AgentPath.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the floor type.
		 */
		class WP_WAYFINDER_API Floor
		{
			/**
			 * @brief Represents the isolated area type.
			 */
			struct IsolatedArea
			{
				std::vector<Vector2> border;
				std::vector<std::vector<Vector2>> holes;
			};

		private:

			int mInset;

			std::vector<Vector2> mBorder;

			std::vector<std::vector<Vector2>> mHoles;

			ConvexPolygonisation* mPolygons;

			PathDatabase* mPathDatabase;

			/**
			 * @brief Calculates paths.
			 * @param target The target parameter used by the method.
			 * @param recalculate The recalculate parameter used by the method.
			 */
			void calculatePaths(PolygonIndex target, bool recalculate) const;

			void initialiseNavigationData();

		public:

			/**
			 * @brief Performs the floor operation.
			 */
			Floor();

			/**
			 * @brief Performs the floor operation.
			 * @param border The border parameter used by the method.
			 */
			explicit Floor(std::vector<Vector2> const& border);

			/**
			 * @brief Performs the floor operation.
			 * @param border The border parameter used by the method.
			 * @param holes The holes parameter used by the method.
			 */
			Floor(std::vector<Vector2> const& border, std::vector<std::vector<Vector2>> const& holes);

			/**
			 * @brief Creates a zero-inset floor from an existing triangulation.
			 */
			Floor(
				std::vector<Vector2> const& border,
				std::vector<std::vector<Vector2>> const& holes,
				std::vector<Vector2> const& vertices,
				std::vector<Triangle> const& triangles);

			/**
			 * @brief Destroys the floor instance.
			 */
			~Floor();

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
			 * @brief Performs the triangulate operation.
			 * @param inset The inset parameter used by the method.
			 */
			void triangulate(int inset);

			/**
			 * @brief Replaces polygonisation with an existing zero-inset triangulation.
			 */
			void setTriangulation(
				std::vector<Vector2> const& vertices,
				std::vector<Triangle> const& triangles);

			/**
			 * @brief Gets the inset.
			 * @return The requested value or operation result.
			 */
			int getInset() const;

			/**
			 * @brief Performs the compose convex operation.
			 * @param maxDegree The maxDegree parameter used by the method.
			 */
			void composeConvex(int maxDegree = ConvexPolygonisation::MaxPolygonDegree);

			/**
			 * @brief Calculates paths.
			 * @param target The target parameter used by the method.
			 */
			void calculatePaths(PolygonIndex target);

			/**
			 * @brief Performs the recalculate paths operation.
			 * @param target The target parameter used by the method.
			 */
			void recalculatePaths(PolygonIndex target);

			/**
			 * @brief Calculates path lengths.
			 * @param target The target parameter used by the method.
			 * @return The requested value or operation result.
			 */
			std::vector<int> calculatePathLengths(PolygonIndex target) const;

			/**
			 * @brief Gets the path.
			 * @param position Position value used by the method.
			 * @param source The source parameter used by the method.
			 * @param target The target parameter used by the method.
			 * @param startTimer The startTimer parameter used by the method.
			 * @return The requested value or operation result.
			 */
			AgentPath getPath(Vector2 const& position, PolygonIndex source, PolygonIndex target, float startTimer = 0.0f) const;

			/**
			 * @brief Gets the polygonisation.
			 * @return The requested value or operation result.
			 */
			ConvexPolygonisation const* getPolygonisation() const;

			/**
			 * @brief Gets the num polygons.
			 * @return The requested value or operation result.
			 */
			uint32_t getNumPolygons() const;

			/**
			 * @brief Gets the containing polygon.
			 * @param x X coordinate or component value.
			 * @param y Y coordinate or component value.
			 * @return The requested value or operation result.
			 */
			int32_t getContainingPolygon(float x, float y) const;

			/**
			 * @brief Gets the containing polygon.
			 * @param position Position value used by the method.
			 * @return The requested value or operation result.
			 */
			int32_t getContainingPolygon(Vector2 const& position) const;
		};

	} // wayfinder
} // WP_NAMESPACE
