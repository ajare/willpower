#pragma once

#include "willpower/common/Vector2.h"
#include "willpower/common/MathsUtils.h"
#include "willpower/common/AccelerationGrid.h"
#include "willpower/common/Logger.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Types.h"
#include "willpower/wayfinder/FloorLine.h"
#include "willpower/wayfinder/PathDatabase.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{
		class Floor;

		/**
		 * @brief Represents the convex polygonisation type.
		 */
		class WP_WAYFINDER_API ConvexPolygonisation
		{
		public:

			static const unsigned int MaxPolygonDegree = 16;

		public:

			/**
			 * @brief Represents the edge type.
			 */
			struct Edge
			{
				FloorLine line;
				PossibleVertexIndex v[2]; // index into vertex list
				PossiblePolygonIndex p[2]; // index into polygon list

				/**
				 * @brief Performs the edge operation.
				 */
				Edge()
				{
					v[0] = v[1] = p[0] = p[1] = NoIndex;
				}
			};

		private:

			/**
			 * @brief Represents the polygon type.
			 */
			struct Polygon
			{
				VertexIndex vertices[MaxPolygonDegree];
				PolygonIndex neighbours[MaxPolygonDegree];
				uint32_t degree, numNeighbours;

				/**
				 * @brief Performs the polygon operation.
				 * @param numNeighbours The numNeighbours parameter used by the method.
				 */
				Polygon() : numNeighbours(0) {}

				/**
				 * @brief Gets the centre.
				 * @param cv The cv parameter used by the method.
				 * @return The requested value or operation result.
				 */
				Vector2 getCentre(ConvexPolygonisation const* cv) const
				{
					Vector2 centre(0.0f, 0.0f);
					for (uint32_t i = 0; i < degree; ++i)
					{
						centre += cv->mVertices[vertices[i]];
					}

					return centre / (float)degree;
				}
			};

			/**
			 * @brief Represents the graph edge type.
			 */
			struct GraphEdge
			{
				PolygonIndex target;
				uint32_t distance;
			};

		private:

			std::vector<Vector2> mVertices;

			std::vector<Edge> mEdges;

			std::map<uint32_t, EdgeIndex> mVertexToEdgeLookup;

			std::map<uint32_t, EdgeIndex> mPolygonToEdgeLookup;

			std::vector<Polygon> mPolygons;

			std::vector<std::vector<GraphEdge>> mPathGraph;

			BoundingBox mBounds;

			AccelerationGrid* mPolygonAccelerationGrid;

		private:

			/**
			 * @brief Adds vertex.
			 * @param x X coordinate or component value.
			 * @param y Y coordinate or component value.
			 * @return The requested value or operation result.
			 */
			VertexIndex addVertex(float x, float y);

			/**
			 * @brief Adds triangle.
			 * @param vertices The vertices parameter used by the method.
			 * @return The requested value or operation result.
			 */
			uint32_t addTriangle(VertexIndex vertices[3]);

			/**
			 * @brief Performs the process loop operation.
			 * @param points The points parameter used by the method.
			 * @return The requested value or operation result.
			 */
			std::vector<Vector2> processLoop(std::vector<Vector2> const& points);

			/**
			 * @brief Performs the polygonise1 operation.
			 * @param border The border parameter used by the method.
			 * @param holes The holes parameter used by the method.
			 */
			void polygonise1(std::vector<Vector2> const& border, std::vector<std::vector<Vector2>> const& holes);

			/**
			 * @brief Performs the polygonise2 operation.
			 * @param border The border parameter used by the method.
			 * @param holes The holes parameter used by the method.
			 */
			void polygonise2(std::vector<Vector2> const& border, std::vector<std::vector<Vector2>> const& holes);

		public:

			/**
			 * @brief Performs the convex polygonisation operation.
			 */
			ConvexPolygonisation();

			/**
			 * @brief Destroys the convex polygonisation instance.
			 */
			~ConvexPolygonisation();

			/**
			 * @brief Adds area.
			 * @param border The border parameter used by the method.
			 * @param holes The holes parameter used by the method.
			 */
			void addArea(std::vector<Vector2> const& border, std::vector<std::vector<Vector2>> const& holes);

			/**
			 * @brief Adds an existing triangulation without triangulating it again.
			 * @param vertices Vertex positions used by triangles.
			 * @param triangles Triangle vertex indices.
			 */
			void addTriangulation(std::vector<Vector2> const& vertices, std::vector<Triangle> const& triangles);

			/**
			 * @brief Performs the build path graph operation.
			 */
			void buildPathGraph();

			/**
			 * @brief Creates acceleration grids.
			 * @param dimX The dimX parameter used by the method.
			 * @param dimY The dimY parameter used by the method.
			 */
			void createAccelerationGrids(int dimX, int dimY);

			/**
			 * @brief Calculates paths.
			 * @param target The target parameter used by the method.
			 * @param database The database parameter used by the method.
			 */
			void calculatePaths(PolygonIndex target, PathDatabase* database) const;

			/**
			 * @brief Calculates path lengths.
			 * @param target The target parameter used by the method.
			 * @return The requested value or operation result.
			 */
			std::vector<int> calculatePathLengths(PolygonIndex target) const;

			/**
			 * @brief Performs the compose convex operation.
			 * @param maxDegree The maxDegree parameter used by the method.
			 */
			void composeConvex(unsigned int maxDegree = MaxPolygonDegree);

			/**
			 * @brief Gets the vertex.
			 * @param index Zero-based index to read or write.
			 * @return The requested value or operation result.
			 */
			Vector2 const& getVertex(VertexIndex index) const;

			/**
			 * @brief Gets the edge.
			 * @param index Zero-based index to read or write.
			 * @return The requested value or operation result.
			 */
			Edge const& getEdge(EdgeIndex index) const;

			/**
			 * @brief Gets the polygons.
			 * @return The requested value or operation result.
			 */
			std::vector<Polygon> const& getPolygons() const;

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
