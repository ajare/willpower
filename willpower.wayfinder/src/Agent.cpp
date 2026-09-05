#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/wayfinder/Agent.h"
#include "willpower/wayfinder/AgentSwarm.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		Agent::Agent(int radius, Vector2 const& position, float maxSpeed)
			: AgentTarget(position)
			, mDirection(Vector2::UNIT_Y)
			, mSwarm(nullptr)
			, mSector(nullptr)
			, mMaxSpeed(maxSpeed)
			, mTurnRate(90.0f)
			, mRadius(radius)
			, mTarget(nullptr)
		{
		}

		Agent::~Agent()
		{
		}

		Vector2 const& Agent::getPosition() const
		{
			return mPosition;
		}

		Vector2 const& Agent::getDirection() const
		{
			return mDirection;
		}

		void Agent::setSwarm(AgentSwarm* swarm)
		{
			mSwarm = swarm;
		}

		void Agent::setAgentId(uint32_t id)
		{
			mId = id;
		}

		uint32_t Agent::getAgentId() const
		{
			return mId;
		}

		int Agent::getRadius() const
		{
			return mRadius;
		}

		void Agent::setMaxSpeed(float maxSpeed)
		{
			mMaxSpeed = maxSpeed;
		}

		float Agent::getMaxSpeed() const
		{
			return mMaxSpeed;
		}

		void Agent::setTurnRate(float turnRate)
		{
			mTurnRate = turnRate;
		}

		float Agent::getTurnRate() const
		{
			return mTurnRate;
		}

		void Agent::setDirection(Vector2 const& direction)
		{
			mDirection = direction;
		}

		void Agent::move(float distance)
		{
			mPosition += mDirection * distance;
		}

		void Agent::setTarget(AgentTarget* target)
		{
			mTarget = target;
		}

		AgentTarget* Agent::getTarget()
		{
			return mTarget;
		}

		void Agent::setPath(AgentPath const& path)
		{
			mPath = path;
			mPath.setTarget(mTarget);
		}

		void Agent::clearPath()
		{
			mPath.clear();
		}

		void Agent::setSector(Sector* sector)
		{
			mSector = sector;
		}

		Sector* Agent::getSector()
		{
			return mSector;
		}

		Vector2 Agent::updatePath(float frameTime)
		{
			return mPath.update(frameTime, mPosition);
		}

	} // wayfinder
} // WP_NAMESPACE
