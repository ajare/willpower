#pragma once

#include <cstdint>
#include <list>
#include <set>
#include <vector>

#include "willpower/geometry/Platform.h"

namespace WP_NAMESPACE {
namespace geometry {

typedef std::list<uint32_t> IndexList;
typedef std::vector<uint32_t> IndexVector;
typedef std::set<uint32_t> IndexSet;

}  // namespace geometry
}  // namespace WP_NAMESPACE
