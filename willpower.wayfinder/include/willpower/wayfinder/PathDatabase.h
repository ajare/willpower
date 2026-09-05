#pragma once


#include "willpower/wayfinder/Platform.h"
#include "willpower/wayfinder/Types.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		/**
		 * @brief Represents the path database type.
		 */
		class WP_WAYFINDER_API PathDatabase
		{
			friend class ConvexPolygonisation;

		public:

			/**
			 * @brief Represents the entry type.
			 */
			struct Entry
			{
				EdgeIndex* base;
				uint32_t length;

			public:

				/**
				 * @brief Performs the entry operation.
				 * @param nullptr The nullptr parameter used by the method.
				 * @param length The length parameter used by the method.
				 */
				Entry()
					: base(nullptr)
					, length(0)
				{
				}
			};

		private:

			EdgeIndex* mData;

			EdgeIndex* mpHead;

			Entry* mEntries;

			std::vector<bool> mIsCalculated;

			uint32_t mNumPolygons;

		private:

			/**
			 * @brief Performs the triangular operation.
			 * @param n The n parameter used by the method.
			 * @return The requested value or operation result.
			 */
			inline uint32_t triangular(uint32_t n) const
			{
				return n * (n + 1) / 2;
			}

			/**
			 * @brief Gets the index.
			 * @param source The source parameter used by the method.
			 * @param target The target parameter used by the method.
			 * @return The requested value or operation result.
			 */
			inline uint32_t getIndex(EdgeIndex source, EdgeIndex target) const
			{
				uint32_t source1 = source + 1;
				return triangular(mNumPolygons - 1) - triangular(mNumPolygons - source1) + (target - source1);
			}

			/**
			 * @brief Creates entry.
			 * @param source The source parameter used by the method.
			 * @param target The target parameter used by the method.
			 * @param base The base parameter used by the method.
			 * @param length The length parameter used by the method.
			 */
			void createEntry(PolygonIndex source, PolygonIndex target, EdgeIndex* base, uint32_t length);

			/**
			 * @brief Gets the head.
			 * @return The requested value or operation result.
			 */
			EdgeIndex* getHead() const;

			/**
			 * @brief Gets the data at offset.
			 * @param offset The offset parameter used by the method.
			 * @return The requested value or operation result.
			 */
			EdgeIndex* getDataAtOffset(uint32_t offset);

		public:

			/**
			 * @brief Performs the path database operation.
			 */
			PathDatabase();

			/**
			 * @brief Destroys the path database instance.
			 */
			~PathDatabase();

			/**
			 * @brief Sets the size.
			 * @param numPolygons The numPolygons parameter used by the method.
			 */
			void setSize(int numPolygons);

			/**
			 * @brief Gets the free space.
			 * @return The requested value or operation result.
			 */
			uint32_t getFreeSpace() const;

			/**
			 * @brief Gets the entry.
			 * @param source The source parameter used by the method.
			 * @param target The target parameter used by the method.
			 * @return The requested value or operation result.
			 */
			Entry const& getEntry(PolygonIndex source, PolygonIndex target) const;

			/**
			 * @brief Sets the calculated.
			 * @param target The target parameter used by the method.
			 */
			void setCalculated(PolygonIndex target);

			/**
			 * @brief Determines whether calculated is true.
			 * @param target The target parameter used by the method.
			 * @return The requested value or operation result.
			 */
			bool isCalculated(PolygonIndex target) const;
		};

	} // wayfinder
} // WP_NAMESPACE
