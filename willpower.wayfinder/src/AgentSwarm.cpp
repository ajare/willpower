#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/wayfinder/AgentSwarm.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		AgentSwarm::AgentSwarm(Mesh* mesh, int initialCapacity)
			: mwMesh(mesh)
			, mAgentCount(0)
			, mAgentGrid(nullptr)
			, mSeekingWeight(1.0f)
			, mSeparationWeight(1.0f)
			, mAlignmentWeight(1.0f)
			, mWallAvoidanceWeight(1.0f)
			, mNeighbourSensorRange(2.0f)
			, mWallSensorRange(2.0f)
			, mAgentPadding(0.0f)
		{
			mAgents.resize(initialCapacity);

			auto meshBounds = mwMesh->getBounds();

			mAgentGrid = new AccelerationGrid(meshBounds.getPosition(), meshBounds.getSize(), 10, 10);
		}

		AgentSwarm::~AgentSwarm()
		{
			delete mAgentGrid;
		}

		void AgentSwarm::addAgent(Agent* agent)
		{
			uint32_t agentId = mAgentCount;
			agent->setAgentId(agentId);
			agent->setSwarm(this);

			// Set agent sector/polygon index
			int size = agent->getRadius();
			Vector2 const& position = agent->getPosition();
			auto sector = mwMesh->getContainingSector(size, position);

			agent->setSector(sector);

			mwAddedAgents.push_back(agent);

			// Add to acceleration grid
			BoundingBox agentBounds;

			agentBounds.setPosition(position.x - size, position.y - size);
			agentBounds.setSize(size * 2.0f, size * 2.0f);

			mAgentGrid->addItem(agentId, agentBounds);
		}

		void AgentSwarm::removeAgent(Agent* agent)
		{
			uint32_t id = agent->getAgentId();
			mAgents[id].agent = nullptr;

			// Remove from acceleration grid
			mAgentGrid->removeItem(id);
		}

		void AgentSwarm::setAgentTarget(Agent* agent, AgentTarget* target)
		{
			AgentTarget* curTarget = agent->getTarget();

			if (curTarget == target)
			{
				return;
			}

			// Remove old target
			if (curTarget)
			{
				// Decrement count on current target
				auto targetIt = mTargets.find(curTarget);

				auto& agentTargetData = targetIt->second;

				agentTargetData.refCount--;

				if (agentTargetData.refCount == 0)
				{
					mTargets.erase(targetIt);
				}
				else
				{
					// Even if we don't remove the agent, we need to remove
					// a size reference.
					int agentRadius = agent->getRadius();
					auto sizeIt = agentTargetData.sizeCounts.find(agentRadius);

					auto& refCount = sizeIt->second.refCount;
					refCount--;

					if (refCount == 0)
					{
						agentTargetData.sizeCounts.erase(sizeIt);
					}
				}
			}

			// Add new target
			agent->setTarget(target);

			if (target)
			{
				int agentRadius = agent->getRadius();

				// Update target database
				auto targetIt = mTargets.find(target);
				if (targetIt != mTargets.end())
				{
					auto& agentTargetData = targetIt->second;

					agentTargetData.refCount++;

					// Add reference to agent size
					auto sizeIt = agentTargetData.sizeCounts.find(agentRadius);

					if (sizeIt != agentTargetData.sizeCounts.end())
					{
						auto& refCount = sizeIt->second.refCount;
						refCount++;
					}
					else
					{
						agentTargetData.sizeCounts[agentRadius] = AgentTargetData::SectorData();
					}
				}
				else
				{
					auto targetData = AgentTargetData();

					// Add reference to agent size
					targetData.sizeCounts[agentRadius] = AgentTargetData::SectorData();
					mTargets[target] = targetData;
				}
			}
		}

		uint32_t AgentSwarm::getNumAgents() const
		{
			return mAgentCount;
		}

		Agent const* AgentSwarm::getAgent(uint32_t index) const
		{
			return mAgents[index].agent;
		}

		Agent* AgentSwarm::getAgent(uint32_t index)
		{
			return mAgents[index].agent;
		}

		AccelerationGrid const* AgentSwarm::getAccelerationGrid() const
		{
			return mAgentGrid;
		}

		set<uint32_t> AgentSwarm::getAgentIdsWithinRadius(Vector2 const& position, float radius) const
		{
			BoundingCircle bc;
			bc.setPosition(position);
			bc.setRadius(radius);

			AccelerationGrid::IndexCollection indices;
			mAgentGrid->getCandidateItemsInBoundingArea(bc, indices);
			return set<uint32_t>(indices.begin(), indices.end());
		}

		void AgentSwarm::setAgentFlockSeekingWeight(float weight)
		{
			mSeekingWeight = weight;
		}

		float AgentSwarm::getAgentFlockSeekingWeight() const
		{
			return mSeekingWeight;
		}

		void AgentSwarm::setAgentFlockSeparationWeight(float weight)
		{
			mSeparationWeight = weight;
		}

		float AgentSwarm::getAgentFlockSeparationWeight() const
		{
			return mSeparationWeight;
		}

		void AgentSwarm::setAgentFlockAlignmentWeight(float weight)
		{
			mAlignmentWeight = weight;
		}

		float AgentSwarm::getAgentFlockAlignmentWeight() const
		{
			return mAlignmentWeight;
		}

		void AgentSwarm::setAgentFlockWallAvoidanceWeight(float weight)
		{
			mWallAvoidanceWeight = weight;
		}

		float AgentSwarm::getAgentFlockWallAvoidanceWeight() const
		{
			return mWallAvoidanceWeight;
		}

		void AgentSwarm::setAgentFlockNeighbourSensorRange(float range)
		{
			mNeighbourSensorRange = range;
		}

		float AgentSwarm::getAgentFlockNeighbourSensorRange() const
		{
			return mNeighbourSensorRange;
		}

		void AgentSwarm::setAgentFlockWallSensorRange(float range)
		{
			mWallSensorRange = range;
		}

		float AgentSwarm::getAgentFlockWallSensorRange() const
		{
			return mWallSensorRange;
		}

		void AgentSwarm::setAgentFlockPadding(float padding)
		{
			mAgentPadding = padding;
		}

		float AgentSwarm::getAgentFlockPadding() const
		{
			return mAgentPadding;
		}

		void AgentSwarm::addNewAgents()
		{
			size_t capacity = mAgents.capacity();
			size_t targetSize = mAgentCount + mwAddedAgents.size();

			if (targetSize > capacity)
			{
				size_t doubleSize = capacity * 2;
				size_t newSize = targetSize > (doubleSize) ? targetSize : doubleSize;
				mAgents.resize(newSize);
			}

			for (size_t i = 0; i < mwAddedAgents.size(); ++i)
			{
				Agent* agent = mwAddedAgents[i];
				int agentRadius = agent->getRadius();

				// Set up agent data
				AgentData& ad = mAgents[mAgentCount];

				ad.agent = agent;
				ad.sector = mwMesh->getContainingSector(agentRadius, ad.agent->getPosition());

				if (!ad.sector->hasFloor(agentRadius))
				{
					ad.floor = ad.sector->createFloor(agentRadius);
				}
				else
				{
					ad.floor = ad.sector->getFloor(ad.agent->getRadius());
				}

				mAgentCount++;
			}

			mwAddedAgents.clear();
		}

		void AgentSwarm::calculateTargetPaths()
		{
			// Set up paths for agents
			for (auto& targetEntry: mTargets)
			{
				auto target = targetEntry.first;
				auto& targetData = targetEntry.second;
				auto const& targetPosition = target->getPosition();

				// Get the Sector of all agents, and the polygon that they are on in
				// that sector, for each size.
				for (auto& sizeEntry: targetData.sizeCounts)
				{
					int size = sizeEntry.first;

					Sector* sector = mwMesh->getContainingSector(size, targetPosition);

					if (!sector)
					{
						sizeEntry.second.sector = NoIndex;

						if (sizeEntry.second.polygon != NoIndex)
						{
							sizeEntry.second.moved = true;
						}

						sizeEntry.second.polygon = NoIndex;
					}
					else
					{
						sizeEntry.second.sector = sector->getId();

						Floor* floor = sector->getFloor(size);

						int32_t floorPoly = floor->getContainingPolygon(targetPosition);

						sizeEntry.second.moved = floorPoly != sizeEntry.second.polygon;
						sizeEntry.second.polygon = floorPoly;

						floor->calculatePaths((PolygonIndex)floorPoly);
					}
				}
			}
		}

		void AgentSwarm::updateAgents(float frameTime)
		{
			uint32_t aCount = 0;
			for (uint32_t i = 0; i < mAgentCount; ++i)
			{
				auto& agentData = mAgents[i];
				Agent* agent = agentData.agent;

				auto const& agentPosition = agent->getPosition();
				int agentRadius = agent->getRadius();

				if (agent)
				{
					auto target = agent->getTarget();
					if (target)
					{
						auto prevTarget = agentData.target;

						auto const& targetData = mTargets[target];
						auto const& sectorData = targetData.sizeCounts.at(agentRadius);

						// Assume agent and target are in the same sector.  Update agent path
						// if the Target has changed polygon, or if the Agent has changed Target.
						// This also catches the case of the Agent being initialised and going
						// from having no target to having a target.
						if (sectorData.moved || target != prevTarget)
						{
							if (sectorData.sector == NoIndex)
							{
								// For now, stop.
								agent->clearPath();
							}
							else
							{
								auto agentSector = agent->getSector();
								Floor* agentFloor = agentSector->getFloor(agentRadius);
								PolygonIndex targetPolygon = (PolygonIndex)sectorData.polygon;

								PossiblePolygonIndex agentPolygon = agentFloor->getContainingPolygon(agentPosition);

								AgentPath agentPath = agentFloor->getPath(agentPosition, (PolygonIndex)agentPolygon, targetPolygon);
								agentPath.setTarget(agent->getTarget());
								agent->setPath(agentPath);
							}
						}
						else if (!agentData.target && prevTarget)
						{
							// If no target, but used to have one, then stop.
							agent->clearPath();
						}
					}

					// Update target info
					agentData.target = target;

					agent->update(frameTime);

					// Update on grid
					BoundingBox agentBounds;

					agentBounds.setPosition(agentPosition.x - agentRadius, agentPosition.y - agentRadius);
					agentBounds.setSize(agentRadius * 2.0f, agentRadius * 2.0f);

					// Reasonable to assume the agent is moving, can always add
					// a simple check in later.
					mAgentGrid->moveItem(agent->getAgentId(), agentBounds);

					if (i != aCount)
					{
						mAgents[aCount] = mAgents[i];
						mAgents[aCount].agent->setAgentId(aCount);
					}

					aCount++;
				}
			}

			mAgentCount = aCount;
		}

		void AgentSwarm::update(float frameTime)
		{
			addNewAgents();
			calculateTargetPaths();
			updateAgents(frameTime);
		}

	} // wayfinder
} // WP_NAMESPACE
