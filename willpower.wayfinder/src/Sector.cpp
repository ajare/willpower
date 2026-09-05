#include <algorithm>

#include <utils/StringUtils.h>

#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/geometry/MeshUtils.h"

#include "willpower/wayfinder/Sector.h"

#undef min
#undef max

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		Sector::Sector()
			: mId(-1)
			, mEdgesGrid(nullptr)
		{
			mBounds.setPosition(numeric_limits<float>::max(), numeric_limits<float>::max());
			mBounds.setSize(numeric_limits<float>::lowest(), numeric_limits<float>::lowest());
		}

		Sector::Sector(vector<Vector2> const& border)
			: mBorder(border)
			, mId(-1)
			, mEdgesGrid(nullptr)
		{
			mBounds.setPosition(numeric_limits<float>::max(), numeric_limits<float>::max());
			mBounds.setSize(numeric_limits<float>::lowest(), numeric_limits<float>::lowest());
		}

		Sector::Sector(vector<Vector2> const& border, vector<vector<Vector2>> const& holes)
			: mBorder(border)
			, mHoles(holes)
			, mId(-1)
			, mEdgesGrid(nullptr)
		{
			mBounds.setPosition(numeric_limits<float>::max(), numeric_limits<float>::max());
			mBounds.setSize(numeric_limits<float>::lowest(), numeric_limits<float>::lowest());
		}

		Sector::~Sector()
		{
			for (auto floor: mFloors)
			{
				delete floor.second;
			}

			delete mEdgesGrid;
		}

		void Sector::setId(int32_t id)
		{
			mId = id;
		}

		int32_t Sector::getId() const
		{
			return mId;
		}

		void Sector::updateBounds(vector<Vector2> const& points)
		{
			Vector2 minExtent, maxExtent;
			mBounds.getExtents(minExtent, maxExtent);

			for (auto const& point: points)
			{
				if (point.x < minExtent.x)
				{
					minExtent.x = point.x;
				}
				if (point.y < minExtent.y)
				{
					minExtent.y = point.y;
				}
				if (point.x > maxExtent.x)
				{
					maxExtent.x = point.x;
				}
				if (point.y > maxExtent.y)
				{
					maxExtent.y = point.y;
				}
			}

			mBounds.setPosition(minExtent);
			mBounds.setSize(maxExtent - minExtent);
		}

		void Sector::setBorder(vector<Vector2> const& border)
		{
			mBorder = border;
			updateBounds(border);
		}

		void Sector::addHole(vector<Vector2> const& hole)
		{
			mHoles.push_back(hole);
			updateBounds(hole);
		}

		void Sector::addHoles(vector<vector<Vector2>> const& holes)
		{
			for (auto const& hole: holes)
			{
				addHole(hole);
			}
		}

		vector<Vector2> const& Sector::getBorder() const
		{
			return mBorder;
		}

		uint32_t Sector::getNumHoles() const
		{
			return (uint32_t)mHoles.size();
		}

		vector<Vector2> const& Sector::getHole(uint32_t index) const
		{
			ASSERT_TRACE(index < mHoles.size() && "Sector::getHole(): index is out of range.");
			return mHoles[index];
		}

		vector<Vector2> Sector::getEdgeVerticesWithinRadius(Vector2 const& position, float radius) const
		{
			vector<Vector2> edgeVertices;

			BoundingCircle bc;
			bc.setPosition(position);
			bc.setRadius(radius);

			AccelerationGrid::IndexCollection indices;
			mEdgesGrid->getCandidateItemsInBoundingArea(bc, indices);

			for (auto index: indices)
			{
				edgeVertices.push_back(mEdgeList[index]);
				edgeVertices.push_back(mEdgeList[index + 1]);
				edgeVertices.push_back(mEdgeList[index + 2]);
			}

			return edgeVertices;
		}

		void Sector::createAccelerationGrid()
		{
			delete mEdgesGrid;

			mEdgesGrid = new AccelerationGrid(mBounds.getPosition(), mBounds.getSize(), 10, 10);

			mEdgeList.clear();

			auto size = (uint32_t)mBorder.size();
			for (uint32_t i = 0; i < size; ++i)
			{
				uint32_t j = (i + 1) % size;

				auto index = (uint32_t)mEdgeList.size();

				mEdgeList.push_back(mBorder[i]);
				mEdgeList.push_back(mBorder[j]);
				mEdgeList.push_back((mBorder[j] - mBorder[i]).perpendicular().normalisedCopy()); // Normal

				BoundingBox bounds;

				Vector2 boundsMin(std::min(mBorder[i].x, mBorder[j].x), std::min(mBorder[i].y, mBorder[j].y));
				Vector2 boundsMax(std::max(mBorder[i].x, mBorder[j].x), std::max(mBorder[i].y, mBorder[j].y));

				bounds.setPosition(boundsMin);
				bounds.setSize(boundsMax - boundsMin);
				mEdgesGrid->addItem(index, bounds);
			}

			for (auto const& hole: mHoles)
			{
				size = (uint32_t)hole.size();
				for (uint32_t i = 0; i < size; ++i)
				{
					uint32_t j = (i + 1) % size;

					auto index = (uint32_t)mEdgeList.size();

					mEdgeList.push_back(hole[i]);
					mEdgeList.push_back(hole[j]);
					mEdgeList.push_back((hole[j] - hole[i]).perpendicular().normalisedCopy()); // Normal

					BoundingBox bounds;

					Vector2 boundsMin(std::min(hole[i].x, hole[j].x), std::min(hole[i].y, hole[j].y));
					Vector2 boundsMax(std::max(hole[i].x, hole[j].x), std::max(hole[i].y, hole[j].y));

					bounds.setPosition(boundsMin);
					bounds.setSize(boundsMax - boundsMin);
					mEdgesGrid->addItem(index, bounds);
				}
			}
		}

		Floor* Sector::createFloor(int insetSize)
		{
			Floor* floor = new Floor(mBorder, mHoles);
			floor->triangulate(insetSize);

			if (mFloors.find(insetSize) != mFloors.end())
			{
				delete mFloors[insetSize];
			}

			mFloors[insetSize] = floor;
			return floor;
		}

		void Sector::composeConvex(int maxDegree)
		{
			for (auto kvp: mFloors)
			{
				kvp.second->composeConvex(maxDegree);
			}
		}

		vector<int> Sector::getFloorInsetSizes() const
		{
			vector<int> sizes;

			for (auto floor: mFloors)
			{
				sizes.push_back(floor.first);
			}

			return sizes;
		}

		bool Sector::hasFloor(int insetSize) const
		{
			return mFloors.find(insetSize) != mFloors.end();
		}

		Floor* Sector::getFloor(int insetSize)
		{
			return mFloors.at(insetSize);
		}

		bool Sector::containsPoint(int insetSize, float x, float y) const
		{
			Floor const* floor = mFloors.at(insetSize);
			return floor->getContainingPolygon(x, y) >= 0;
		}

	} // wayfinder
} // WP_NAMESPACE
