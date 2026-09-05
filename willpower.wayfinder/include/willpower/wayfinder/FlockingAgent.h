#pragma once

#include "willpower/common/Vector2.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Agent.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the flocking agent type.
		 */
		class WP_WAYFINDER_API FlockingAgent : public Agent
		{
			/**
			 * @brief Gets the neighbour forces.
			 * @param separation The separation parameter used by the method.
			 * @param alignment The alignment parameter used by the method.
			 */
			void getNeighbourForces(Vector2& separation, Vector2& alignment);

			/**
			 * @brief Gets the map forces.
			 * @param wallAvoidance The wallAvoidance parameter used by the method.
			 */
			void getMapForces(Vector2& wallAvoidance);

			/**
			 * @brief Performs the clamp turn operation.
			 * @param newDirection The newDirection parameter used by the method.
			 * @param frameTime Elapsed frame time passed to the operation.
			 * @return The requested value or operation result.
			 */
			Vector2 clampTurn(Vector2 const& newDirection, float frameTime);

		public:

			/**
			 * @brief Performs the flocking agent operation.
			 * @param radius Radius value used by the method.
			 * @param position Position value used by the method.
			 * @param maxSpeed The maxSpeed parameter used by the method.
			 */
			FlockingAgent(int radius, Vector2 const& position, float maxSpeed);

			/**
			 * @brief Destroys the flocking agent instance.
			 */
			~FlockingAgent();

			/**
			 * @brief Updates .
			 * @param frameTime Elapsed frame time passed to the operation.
			 */
			void update(float frameTime);
		};

	} // wayfinder
} // WP_NAMESPACE
