#pragma once

#include "willpower/common/Vector2.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/AgentTarget.h"
#include "willpower/wayfinder/AgentPath.h"
#include "willpower/wayfinder/Sector.h"
#include "willpower/wayfinder/ConvexPolygonisation.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{
		class AgentSwarm;

		/**
		 * @brief Represents the agent type.
		 */
		class WP_WAYFINDER_API Agent : public AgentTarget
		{
			friend class AgentSwarm;

		private:

			uint32_t mId;

			// Properties
			Vector2 mDirection;

			int mRadius;

			float mMaxSpeed;

			float mTurnRate;

			AgentPath mPath;

			// Targetting
			AgentTarget* mTarget;

		protected:

			AgentSwarm* mSwarm;

			Sector* mSector;

		private:

			/**
			 * @brief Sets the swarm.
			 * @param swarm The swarm parameter used by the method.
			 */
			void setSwarm(AgentSwarm* swarm);

			/**
			 * @brief Sets the agent id.
			 * @param id The id parameter used by the method.
			 */
			void setAgentId(uint32_t id);

			/**
			 * @brief Sets the target.
			 * @param target The target parameter used by the method.
			 */
			void setTarget(AgentTarget* target);

			/**
			 * @brief Sets the path.
			 * @param path The path parameter used by the method.
			 */
			void setPath(AgentPath const& path);

			/**
			 * @brief Performs the clear path operation.
			 */
			void clearPath();

			/**
			 * @brief Sets the sector.
			 * @param sector The sector parameter used by the method.
			 */
			void setSector(Sector* sector);

		protected:

			/**
			 * @brief Gets the agent id.
			 * @return The requested value or operation result.
			 */
			uint32_t getAgentId() const;

			/**
			 * @brief Gets the sector.
			 * @return The requested value or operation result.
			 */
			Sector* getSector();

			/**
			 * @brief Updates path.
			 * @param frameTime Elapsed frame time passed to the operation.
			 * @return The requested value or operation result.
			 */
			Vector2 updatePath(float frameTime);

			/**
			 * @brief Sets the direction.
			 * @param direction The direction parameter used by the method.
			 */
			void setDirection(Vector2 const& direction);

			/**
			 * @brief Performs the move operation.
			 * @param distance The distance parameter used by the method.
			 */
			void move(float distance);

		public:

			/**
			 * @brief Performs the agent operation.
			 * @param radius Radius value used by the method.
			 * @param position Position value used by the method.
			 * @param maxSpeed The maxSpeed parameter used by the method.
			 */
			Agent(int radius, Vector2 const& position, float maxSpeed);

			/**
			 * @brief Destroys the agent instance.
			 */
			~Agent();

			/**
			 * @brief Gets the position.
			 * @return The requested value or operation result.
			 */
			Vector2 const& getPosition() const;

			/**
			 * @brief Gets the direction.
			 * @return The requested value or operation result.
			 */
			Vector2 const& getDirection() const;

			/**
			 * @brief Gets the radius.
			 * @return The requested value or operation result.
			 */
			int getRadius() const;

			/**
			 * @brief Sets the max speed.
			 * @param maxSpeed The maxSpeed parameter used by the method.
			 */
			void setMaxSpeed(float maxSpeed);

			/**
			 * @brief Gets the max speed.
			 * @return The requested value or operation result.
			 */
			float getMaxSpeed() const;

			/**
			 * @brief Sets the turn rate.
			 * @param turnRate The turnRate parameter used by the method.
			 */
			void setTurnRate(float turnRate);

			/**
			 * @brief Gets the turn rate.
			 * @return The requested value or operation result.
			 */
			float getTurnRate() const;

			/**
			 * @brief Gets the target.
			 * @return The requested value or operation result.
			 */
			AgentTarget* getTarget();

			/**
			 * @brief Updates .
			 * @param frameTime Elapsed frame time passed to the operation.
			 */
			virtual void update(float frameTime) = 0;
		};

	} // wayfinder
} // WP_NAMESPACE
