#pragma once

#include "willpower/common/Vector2.h"

#include "willpower/wayfinder/Platform.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the agent target type.
		 */
		class WP_WAYFINDER_API AgentTarget
		{
		protected:

			Vector2 mPosition;

		public:

			/**
			 * @brief Performs the agent target operation.
			 * @param position Position value used by the method.
			 */
			explicit AgentTarget(Vector2 const& position);

			/**
			 * @brief Destroys the agent target instance.
			 */
			virtual ~AgentTarget();

			/**
			 * @brief Sets the position.
			 * @param position Position value used by the method.
			 */
			void setPosition(Vector2 const& position);

			/**
			 * @brief Gets the position.
			 * @return The requested value or operation result.
			 */
			Vector2 const& getPosition() const;
		};

	} // wayfinder
} // WP_NAMESPACE
