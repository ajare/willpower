#pragma once

#include <cstdint>

#include "willpower/wayfinder/Platform.h"

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		// !! Changing these will break small but crucial bits of code !!
		typedef unsigned short VertexIndex;
		typedef unsigned short EdgeIndex;
		typedef unsigned short PolygonIndex;
		typedef int SectorIndex;

		typedef int PossibleVertexIndex;
		typedef int PossibleEdgeIndex;
		typedef int PossiblePolygonIndex;
		typedef int PossibleSectorIndex;

		static const int NoIndex = -1;

		static const unsigned int MaxVertexIndices = (1 << sizeof(VertexIndex));
		static const unsigned int MaxEdgeIndices = (1 << sizeof(EdgeIndex));
		static const unsigned int MaxPolygonIndices = (1 << sizeof(PolygonIndex));

		static const unsigned int MaxVertexIndex = MaxVertexIndices - 1;
		static const unsigned int MaxEdgeIndex = MaxEdgeIndices - 1;
		static const unsigned int MaxPolygonIndex = MaxPolygonIndices - 1;

	} // wayfinder
} // WP_NAMESPACE
