#include "PlatformTimer.h"

#include <chrono>

namespace WP_NAMESPACE {
namespace application {

namespace {
// Counter frequency in nanoseconds per second; mirrors the division by the
// QPC frequency in the original implementation.
constexpr double kCounterFrequency = 1000000000.0;
}  // namespace

std::int64_t HighResTimer::now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double HighResTimer::intervalSeconds(std::int64_t start, std::int64_t end) {
  return static_cast<double>(end - start) / kCounterFrequency;
}

}  // namespace application
}  // namespace WP_NAMESPACE
