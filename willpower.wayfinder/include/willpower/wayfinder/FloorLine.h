#pragma once

#include "willpower/common/Vector2.h"
#include "willpower/common/MathsUtils.h"
#include "willpower/common/AccelerationGrid.h"
#include "willpower/common/Logger.h"

#include "willpower/wayfinder/Platform.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the floor line type.
		 */
		class WP_WAYFINDER_API FloorLine
		{
		public:

			/**
			 * @brief Performs the floor line operation.
			 */
			FloorLine();

			/**
			 * @brief Destroys the floor line instance.
			 */
			~FloorLine();
		};

	} // wayfinder
} // WP_NAMESPACE
