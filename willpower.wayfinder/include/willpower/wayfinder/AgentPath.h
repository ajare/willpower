#pragma once

#include <cstddef>
#include <vector>

#include "willpower/common/Vector2.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Types.h"
#include "willpower/wayfinder/AgentTarget.h"
#include "willpower/wayfinder/ConvexPolygonisation.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the agent path type.
		 */
		class WP_WAYFINDER_API AgentPath
		{
			friend class AgentPathIterator;

		private:

			// Targetting
			AgentTarget const* mTarget;

			ConvexPolygonisation const* mPolygons;

			// Path
			std::vector<EdgeIndex> mPathNodes;

			size_t mCurrentPathNode;

			Vector2 mNodeVertices[2];

			// Optional time before we start pathing
			float mStartTimer;

			// Pathing helpers
			wp::MathsUtils::Side mNextEdgeSign;

			PossiblePolygonIndex mTargetLastSeenPolygon;

		private:

			/**
			 * @brief Performs the in final polygon operation.
			 * @return The requested value or operation result.
			 */
			bool inFinalPolygon() const;

			/**
			 * @brief Performs the moved to new node operation.
			 * @param position Position value used by the method.
			 * @return The requested value or operation result.
			 */
			bool movedToNewNode(Vector2 const& position) const;

			/**
			 * @brief Calculates next edge.
			 * @param position Position value used by the method.
			 * @param node The node parameter used by the method.
			 */
			void calculateNextEdge(Vector2 const& position, EdgeIndex node);

			/**
			 * @brief Gets the direction to target.
			 * @param position Position value used by the method.
			 * @return The requested value or operation result.
			 */
			Vector2 getDirectionToTarget(Vector2 const& position) const;

			/**
			 * @brief Gets the direction to next node.
			 * @param position Position value used by the method.
			 * @return The requested value or operation result.
			 */
			Vector2 getDirectionToNextNode(Vector2 const& position) const;

		public:

			/**
			 * @brief Represents the iterator type.
			 */
			class WP_WAYFINDER_API Iterator
			{
				friend class AgentPath;

			private:

				AgentPath const* mPath;

				size_t mPosition;

			private:

				/**
				 * @brief Performs the iterator operation.
				 */
				Iterator();

				/**
				 * @brief Performs the iterator operation.
				 * @param path The path parameter used by the method.
				 * @param position Position value used by the method.
				 */
				Iterator(AgentPath const* path, int position = 0);

			public:

				/**
				 * @brief Performs the operator++ operation.
				 * @return The requested value or operation result.
				 */
				Iterator const& operator++();

				/**
				 * @brief Performs the operator++ operation.
				 * @return The requested value or operation result.
				 */
				Iterator operator++(int);

				/**
				 * @brief Performs the operator* operation.
				 * @return The requested value or operation result.
				 */
				EdgeIndex operator*();

				/**
				 * @brief Performs the operator== operation.
				 * @param other Object to copy from or compare against.
				 * @return The requested value or operation result.
				 */
				bool operator==(Iterator const& other) const;

				/**
				 * @brief Performs the operator!= operation.
				 * @param other Object to copy from or compare against.
				 * @return The requested value or operation result.
				 */
				bool operator!=(Iterator const& other) const;
			};

		public:

			/**
			 * @brief Performs the agent path operation.
			 */
			AgentPath();

			/**
			 * @brief Performs the agent path operation.
			 * @param position Position value used by the method.
			 * @param pathStart The pathStart parameter used by the method.
			 * @param pathEnd The pathEnd parameter used by the method.
			 * @param pathInc The pathInc parameter used by the method.
			 * @param polygons The polygons parameter used by the method.
			 * @param startTimer The startTimer parameter used by the method.
			 */
			AgentPath(Vector2 const& position, EdgeIndex const* pathStart, EdgeIndex const* pathEnd, int pathInc, ConvexPolygonisation const* polygons, float startTimer = 0.0f);

			/**
			 * @brief Creates a path that owns its portal sequence.
			 */
			AgentPath(
				Vector2 const& position,
				std::vector<EdgeIndex> path,
				ConvexPolygonisation const* polygons,
				float startTimer = 0.0f);

			/**
			 * @brief Performs the clear operation.
			 */
			void clear();

			/**
			 * @brief Sets the target.
			 * @param target The target parameter used by the method.
			 */
			void setTarget(AgentTarget const* target);

			/**
			 * @brief Performs the begin operation.
			 * @return The requested value or operation result.
			 */
			Iterator begin() const;

			/**
			 * @brief Performs the end operation.
			 * @return The requested value or operation result.
			 */
			Iterator end() const;

			/**
			 * @brief Updates .
			 * @param frameTime Elapsed frame time passed to the operation.
			 * @param position Position value used by the method.
			 * @return The requested value or operation result.
			 */
			Vector2 update(float frameTime, Vector2 const& position);
		};

	} // wayfinder
} // WP_NAMESPACE
