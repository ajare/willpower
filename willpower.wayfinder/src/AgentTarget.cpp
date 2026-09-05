#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/wayfinder/AgentTarget.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		AgentTarget::AgentTarget(Vector2 const& position)
			: mPosition(position)
		{
		}

		AgentTarget::~AgentTarget()
		{
		}

		void AgentTarget::setPosition(Vector2 const& position)
		{
			mPosition = position;
		}

		Vector2 const& AgentTarget::getPosition() const
		{
			return mPosition;
		}

	} // wayfinder
} // WP_NAMESPACE
