#include <utility>

#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/wayfinder/AgentPath.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		AgentPath::Iterator::Iterator()
			: mPath(nullptr)
			, mPosition(0)
		{
		}

		AgentPath::Iterator::Iterator(AgentPath const* path, int position)
			: mPath(path)
			, mPosition(static_cast<size_t>(position))
		{
		}

		AgentPath::Iterator const& AgentPath::Iterator::operator++()
		{
			if (mPosition < mPath->mPathNodes.size())
			{
				++mPosition;
			}

			return *this;
		}

		AgentPath::Iterator AgentPath::Iterator::operator++(int)
		{
			auto ret = *this;

			this->operator++();

			return ret;
		}

		EdgeIndex AgentPath::Iterator::operator*()
		{
			return mPath->mPathNodes[mPosition];
		}

		bool AgentPath::Iterator::operator==(Iterator const& other) const
		{
			return
				this->mPath == other.mPath &&
				this->mPosition == other.mPosition;
		}

		bool AgentPath::Iterator::operator!=(Iterator const& other) const
		{
			return !(*this == other);
		}

		AgentPath::AgentPath()
			: mTarget(nullptr)
			, mPolygons(nullptr)
			, mCurrentPathNode(0)
			, mStartTimer(0.0f)
			, mTargetLastSeenPolygon(NoIndex)
		{
		}

		AgentPath::AgentPath(Vector2 const& position, EdgeIndex const* pathStart, EdgeIndex const* pathEnd, int pathInc, ConvexPolygonisation const* polygons, float startTimer)
			: AgentPath()
		{
			mStartTimer = startTimer;
			mPolygons = polygons;
			if (pathStart && pathEnd && pathInc != 0)
			{
				for (auto node = pathStart;; node += pathInc)
				{
					mPathNodes.push_back(*node);
					if (node == pathEnd)
					{
						break;
					}
				}
			}
			if (!mPathNodes.empty())
			{
				calculateNextEdge(position, mPathNodes.front());
			}
		}

		AgentPath::AgentPath(
			Vector2 const& position, vector<EdgeIndex> path,
			ConvexPolygonisation const* polygons, float startTimer)
			: mTarget(nullptr)
			, mPolygons(polygons)
			, mPathNodes(std::move(path))
			, mCurrentPathNode(0)
			, mStartTimer(startTimer)
			, mTargetLastSeenPolygon(NoIndex)
		{
			if (!mPathNodes.empty())
			{
				calculateNextEdge(position, mPathNodes.front());
			}
		}

		void AgentPath::clear()
		{
			mPathNodes.clear();
			mCurrentPathNode = 0;
			mStartTimer = 0.0f;

			mTargetLastSeenPolygon = NoIndex;
			mTarget = nullptr;
			mPolygons = nullptr;
		}

		void AgentPath::setTarget(AgentTarget const* target)
		{
			mTarget = target;
		}

		AgentPath::Iterator AgentPath::begin() const
		{
			return Iterator(this);
		}

		AgentPath::Iterator AgentPath::end() const
		{
			return Iterator(this, static_cast<int>(mPathNodes.size()));
		}

		bool AgentPath::inFinalPolygon() const
		{
			return mCurrentPathNode >= mPathNodes.size();
		}

		void AgentPath::calculateNextEdge(Vector2 const& position, EdgeIndex node)
		{
			auto const& edge = mPolygons->getEdge(node);

			mNodeVertices[0] = mPolygons->getVertex((VertexIndex)edge.v[0]);
			mNodeVertices[1] = mPolygons->getVertex((VertexIndex)edge.v[1]);
			mNextEdgeSign = MathsUtils::pointSideOnLine(position, mNodeVertices[0], mNodeVertices[1]);
		}

		bool AgentPath::movedToNewNode(Vector2 const& position) const
		{
			return MathsUtils::pointSideOnLine(position, mNodeVertices[0], mNodeVertices[1]) != mNextEdgeSign;
		}

		Vector2 AgentPath::getDirectionToTarget(Vector2 const& position) const
		{
			return position.directionTo(mTarget->getPosition());
		}

		Vector2 AgentPath::getDirectionToNextNode(Vector2 const& position) const
		{
			// Get the nearest point on the edge to us.
			//target = position.closestPointOnLine(mNodeVertices[0], mNodeVertices[1]);

			// Get centre of edge
			//target = mNodeVertices[0].lerp(mNodeVertices[1], 0.5f);

			Vector2 targetPosition = mTarget->getPosition();

			// Try and get line of sight.  Project rays through edges and get target side.
			auto side0 = MathsUtils::pointSideOnLine(targetPosition, position, mNodeVertices[0]);
			auto side1 = MathsUtils::pointSideOnLine(targetPosition, position, mNodeVertices[1]);

			if (side0 == side1)
			{
				// Move towards closest vertex
				float d0 = targetPosition.distanceToSq(mNodeVertices[0]);
				float d1 = targetPosition.distanceToSq(mNodeVertices[1]);

				if (d0 > d1)
				{
					return position.directionTo(mNodeVertices[1]);
				}
				else
				{
					return position.directionTo(mNodeVertices[0]);
				}
			}
			else if (side0 == MathsUtils::Side::Left)
			{
				return position.directionTo(targetPosition);
			}
			else
			{
				return position.directionTo(targetPosition);
			}
		}

		Vector2 AgentPath::update(float frameTime, Vector2 const& position)
		{
			// Check to see if we actually have a path
			if (!mTarget)
			{
				return Vector2::ZERO;
			}

			// Don't start if we're counting down
			if (mStartTimer > 0.0f)
			{
				mStartTimer -= frameTime;

				if (mStartTimer > 0.0f)
				{
					return Vector2::ZERO;
				}
			}

			// Process path
			if (inFinalPolygon())
			{
				// In same polygon, aim for target directly.
				return getDirectionToTarget(position);
			}
			else
			{
				if (movedToNewNode(position))
				{
					// Move onto next node
					++mCurrentPathNode;

					if (!inFinalPolygon())
					{
						calculateNextEdge(position, mPathNodes[mCurrentPathNode]);
					}
				}

				if (!inFinalPolygon())
				{
					return getDirectionToNextNode(position);
				}
				else
				{
					return getDirectionToTarget(position);
				}
			}
		}

	} // wayfinder
} // WP_NAMESPACE
