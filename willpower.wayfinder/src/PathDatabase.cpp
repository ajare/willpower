#include <cassert>

#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"

#include "willpower/wayfinder/PathDatabase.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		PathDatabase::PathDatabase()
			: mData(nullptr)
			, mEntries(nullptr)
			, mpHead(nullptr)
		{
		}

		PathDatabase::~PathDatabase()
		{
			delete[] mData;
			delete[] mEntries;
		}

		void PathDatabase::setSize(int numPolygons)
		{
			ASSERT_TRACE(numPolygons > 0 && "PathDatabase::setSize() numPolygons must be > 0.");

			delete[] mData;
			delete[] mEntries;

			mNumPolygons = numPolygons;

			// Create entries
			int numEntries = triangular(mNumPolygons - 1);
			mEntries = new Entry[numEntries];

			// Create data
			int dataSize = (numPolygons * (numPolygons - 1) * (numPolygons + 1)) / 6;
			mData = new EdgeIndex[dataSize];

			mpHead = mData;

			// Create calculated lookup
			mIsCalculated.resize(numPolygons, false);
		}

		void PathDatabase::createEntry(PolygonIndex source, PolygonIndex target, EdgeIndex* base, uint32_t length)
		{
			uint32_t index = getIndex(source, target);
			Entry& entry = mEntries[index];

			entry.base = base;
			entry.length = length;

			mpHead = base + length;
		}

		EdgeIndex* PathDatabase::getHead() const
		{
			return mpHead;
		}

		EdgeIndex* PathDatabase::getDataAtOffset(uint32_t offset)
		{
			return mData + offset;
		}

		uint32_t PathDatabase::getFreeSpace() const
		{
			int dataSize = (mNumPolygons * (mNumPolygons - 1) * (mNumPolygons + 1)) / 6;
			return (uint32_t)distance(mpHead, mData + dataSize);
		}

		PathDatabase::Entry const& PathDatabase::getEntry(PolygonIndex source, PolygonIndex target) const
		{
			if (source > target)
			{
				swap(source, target);
			}

			uint32_t index = getIndex(source, target);
			return mEntries[index];
		}

		void PathDatabase::setCalculated(PolygonIndex target)
		{
			mIsCalculated[target] = true;
		}

		bool PathDatabase::isCalculated(PolygonIndex target) const
		{
			return mIsCalculated[target];
		}

	} // wayfinder
} // WP_NAMESPACE
