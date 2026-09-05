#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/wayfinder/FlockingAgent.h"
#include "willpower/wayfinder/AgentSwarm.h"

#undef min
#undef max

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		FlockingAgent::FlockingAgent(int radius, Vector2 const& position, float maxSpeed)
			: Agent(radius, position, maxSpeed)
		{
		}

		FlockingAgent::~FlockingAgent()
		{
		}

		void FlockingAgent::getNeighbourForces(Vector2& separation, Vector2& alignment)
		{
			float neighbourThreshold = mSwarm->getAgentFlockNeighbourSensorRange() * getRadius();

			Vector2 position = getPosition();
			auto neighbourIds = mSwarm->getAgentIdsWithinRadius(position, neighbourThreshold);
			uint32_t neighbourCount = 0;

			float threshold = neighbourThreshold + mSwarm->getAgentFlockPadding();

			separation = Vector2::ZERO;
			alignment = Vector2::ZERO;
			for (auto id : neighbourIds)
			{
				if (id == getAgentId())
				{
					continue;
				}

				auto const* agent = mSwarm->getAgent(id);
				auto const& agentPosition = agent->getPosition();

				if (position.distanceToSq(agentPosition) <= threshold * threshold)
				{
					// Stay away from agents that might get too close.  Scale inversely
					// to distance: the closer something else, the more we want to push
					// away from it
					Vector2 seperationVector = position - agentPosition;
					float separationDistance = (float)seperationVector.normalise();

					separation += seperationVector * (threshold - separationDistance);

					// Align with nearby agents - if they have the same target.
					alignment += agent->getDirection();

					neighbourCount++;
				}
			}

			if (neighbourCount > 0)
			{
				separation.normalise();
				alignment /= neighbourCount;
			}
		}

		void FlockingAgent::getMapForces(Vector2& wallAvoidance)
		{
			Vector2 position = getPosition();

			float wallThreshold = mSwarm->getAgentFlockWallSensorRange() * getRadius();
			float threshold = wallThreshold + mSwarm->getAgentFlockPadding();
			auto wallVertices = mSector->getEdgeVerticesWithinRadius(position, threshold);

			wallAvoidance = Vector2::ZERO;
			for (uint32_t i = 0; i < wallVertices.size(); i += 3)
			{
				float capDistance = position.distanceToCapsule(wallVertices[i], wallVertices[i + 1], threshold);
				if (capDistance < 0)
				{
					// Move away from wall
					wallAvoidance += wallVertices[i + 2] * -capDistance;
				}
			}

			if (!wallVertices.empty())
			{
				wallAvoidance.normalise();
			}
		}

		Vector2 FlockingAgent::clampTurn(Vector2 const& newDirection, float frameTime)
		{
			Vector2 curDirection = getDirection();
			float turnRate = curDirection.clockwiseAngleTo(newDirection);
			if (turnRate > 180)
			{
				turnRate -= 360;
			}

			// Don't let the agent turn too quickly, will 'flicker' otherwise.
			float turnRateMax = getTurnRate() * frameTime;
			if (fabs(turnRate) > turnRateMax)
			{
				return curDirection.rotatedClockwiseCopy(turnRate > 0 ? turnRateMax : -turnRateMax);
			}
			else
			{
				return newDirection.normalisedCopy();
			}
		}

		void FlockingAgent::update(float frameTime)
		{
			Vector2 seeking = updatePath(frameTime);

			Vector2 separation, alignment;
			getNeighbourForces(separation, alignment);

			Vector2 wallAvoidance;
			getMapForces(wallAvoidance);

			Vector2 direction =
				seeking * mSwarm->getAgentFlockSeekingWeight() +
				separation * mSwarm->getAgentFlockSeparationWeight() +
				alignment * mSwarm->getAgentFlockAlignmentWeight() +
				wallAvoidance * mSwarm->getAgentFlockWallAvoidanceWeight();

			float dirMag = max(0.0f, min(direction.length(), 1.0f));

			// Direction's magnitude should be clamped at within [0, 1], rather than simply normalised.  This should then
			// be used to modify movement speed.  This lets agents slow down if they need to, but not go beyond their max
			// speed.
			if (direction != Vector2::ZERO)
			{
				setDirection(clampTurn(direction, frameTime));
			}

			move(getMaxSpeed() * dirMag * frameTime);
		}

	} // wayfinder
} // WP_NAMESPACE
