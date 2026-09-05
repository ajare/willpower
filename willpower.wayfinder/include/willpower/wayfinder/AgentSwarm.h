#pragma once

#include <map>
#include <unordered_set>

#include "willpower/common/Vector2.h"
#include "willpower/common/AccelerationGrid.h"

#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Types.h"
#include "willpower/wayfinder/Agent.h"
#include "willpower/wayfinder/Mesh.h"
#include "willpower/wayfinder/PathDatabase.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the agent swarm type.
		 */
		class WP_WAYFINDER_API AgentSwarm
		{
			/**
			 * @brief Represents the agent data type.
			 */
			struct AgentData
			{
				// Non-owning
				Agent* agent;
				Sector* sector;
				Floor* floor;
				AgentTarget* target;

				/**
				 * @brief Performs the agent data operation.
				 * @param nullptr The nullptr parameter used by the method.
				 * @param nullptr The nullptr parameter used by the method.
				 * @param nullptr The nullptr parameter used by the method.
				 * @param nullptr The nullptr parameter used by the method.
				 */
				AgentData()
					: agent(nullptr)
					, sector(nullptr)
					, floor(nullptr)
					, target(nullptr)
				{
				}
			};

			/**
			 * @brief Represents the agent target data type.
			 */
			struct AgentTargetData
			{
				/**
				 * @brief Represents the sector data type.
				 */
				struct SectorData
				{
					PossibleSectorIndex sector;
					PossiblePolygonIndex polygon;
					uint32_t refCount;
					bool moved;

					/**
					 * @brief Performs the sector data operation.
					 * @param NoIndex The NoIndex parameter used by the method.
					 * @param NoIndex The NoIndex parameter used by the method.
					 * @param refCount The refCount parameter used by the method.
					 * @param true The true parameter used by the method.
					 */
					SectorData()
						: sector(NoIndex)
						, polygon(NoIndex)
						, refCount(1)
						, moved(true)
					{
					}
				};

				// First in pair is count, second is polygon id for that Floor.
				std::map<int, SectorData> sizeCounts;

				uint32_t refCount;

				/**
				 * @brief Performs the agent target data operation.
				 * @param refCount The refCount parameter used by the method.
				 */
				AgentTargetData()
					: refCount(1)
				{
				}
			};

		private:

			// Mesh data
			Mesh* mwMesh;

			// Agent data
			std::vector<AgentData> mAgents;

			uint32_t mAgentCount;

			std::vector<Agent*> mwAddedAgents;

			// Lookup grid for agents
			AccelerationGrid* mAgentGrid;

			// Agent target data
			std::map<AgentTarget const*, AgentTargetData> mTargets;

			// Flocking params
			float mSeekingWeight;

			float mSeparationWeight;

			float mAlignmentWeight;

			float mWallAvoidanceWeight;

			float mNeighbourSensorRange;

			float mWallSensorRange;

			float mAgentPadding;

		private:

			/**
			 * @brief Adds new agents.
			 */
			void addNewAgents();

			/**
			 * @brief Calculates target paths.
			 */
			void calculateTargetPaths();

			/**
			 * @brief Updates agents.
			 * @param frameTime Elapsed frame time passed to the operation.
			 */
			void updateAgents(float frameTime);

		public:

			/**
			 * @brief Performs the agent swarm operation.
			 * @param mesh Mesh instance to operate on.
			 * @param initialCapacity The initialCapacity parameter used by the method.
			 */
			AgentSwarm(Mesh* mesh, int initialCapacity);

			/**
			 * @brief Destroys the agent swarm instance.
			 */
			~AgentSwarm();

			/**
			 * @brief Adds agent.
			 * @param agent The agent parameter used by the method.
			 */
			void addAgent(Agent* agent);

			/**
			 * @brief Removes agent.
			 * @param agent The agent parameter used by the method.
			 */
			void removeAgent(Agent* agent);

			/**
			 * @brief Gets the num agents.
			 * @return The requested value or operation result.
			 */
			uint32_t getNumAgents() const;

			/**
			 * @brief Gets the agent.
			 * @param index Zero-based index to read or write.
			 * @return The requested value or operation result.
			 */
			Agent const* getAgent(uint32_t index) const;

			/**
			 * @brief Gets the agent.
			 * @param index Zero-based index to read or write.
			 * @return The requested value or operation result.
			 */
			Agent* getAgent(uint32_t index);

			/**
			 * @brief Gets the acceleration grid.
			 * @return The requested value or operation result.
			 */
			AccelerationGrid const* getAccelerationGrid() const;

			/**
			 * @brief Gets the agent ids within radius.
			 * @param position Position value used by the method.
			 * @param radius Radius value used by the method.
			 * @return The requested value or operation result.
			 */
			std::set<uint32_t> getAgentIdsWithinRadius(Vector2 const& position, float radius) const;

			/**
			 * @brief Sets the agent target.
			 * @param agent The agent parameter used by the method.
			 * @param target The target parameter used by the method.
			 */
			void setAgentTarget(Agent* agent, AgentTarget* target);

			/**
			 * @brief Sets the agent flock seeking weight.
			 * @param weight The weight parameter used by the method.
			 */
			void setAgentFlockSeekingWeight(float weight);

			/**
			 * @brief Gets the agent flock seeking weight.
			 * @return The requested value or operation result.
			 */
			float getAgentFlockSeekingWeight() const;

			/**
			 * @brief Sets the agent flock separation weight.
			 * @param weight The weight parameter used by the method.
			 */
			void setAgentFlockSeparationWeight(float weight);

			/**
			 * @brief Gets the agent flock separation weight.
			 * @return The requested value or operation result.
			 */
			float getAgentFlockSeparationWeight() const;

			/**
			 * @brief Sets the agent flock alignment weight.
			 * @param weight The weight parameter used by the method.
			 */
			void setAgentFlockAlignmentWeight(float weight);

			/**
			 * @brief Gets the agent flock alignment weight.
			 * @return The requested value or operation result.
			 */
			float getAgentFlockAlignmentWeight() const;

			/**
			 * @brief Sets the agent flock wall avoidance weight.
			 * @param weight The weight parameter used by the method.
			 */
			void setAgentFlockWallAvoidanceWeight(float weight);

			/**
			 * @brief Gets the agent flock wall avoidance weight.
			 * @return The requested value or operation result.
			 */
			float getAgentFlockWallAvoidanceWeight() const;

			/**
			 * @brief Sets the agent flock neighbour sensor range.
			 * @param range The range parameter used by the method.
			 */
			void setAgentFlockNeighbourSensorRange(float range);

			/**
			 * @brief Gets the agent flock neighbour sensor range.
			 * @return The requested value or operation result.
			 */
			float getAgentFlockNeighbourSensorRange() const;

			/**
			 * @brief Sets the agent flock wall sensor range.
			 * @param range The range parameter used by the method.
			 */
			void setAgentFlockWallSensorRange(float range);

			/**
			 * @brief Gets the agent flock wall sensor range.
			 * @return The requested value or operation result.
			 */
			float getAgentFlockWallSensorRange() const;

			/**
			 * @brief Sets the agent flock padding.
			 * @param padding The padding parameter used by the method.
			 */
			void setAgentFlockPadding(float padding);

			/**
			 * @brief Gets the agent flock padding.
			 * @return The requested value or operation result.
			 */
			float getAgentFlockPadding() const;

			/**
			 * @brief Updates .
			 * @param frameTime Elapsed frame time passed to the operation.
			 */
			void update(float frameTime);

		};

	} // wayfinder
} // WP_NAMESPACE
