#pragma once

#include <cstdint>

#include "willpower/application/Platform.h"

namespace WP_NAMESPACE {
namespace application {

// Small portable monotonic timer replacing the Win32
// QueryPerformanceFrequency()/QueryPerformanceCounter() pair previously used
// by Scheduler and SchedulerTask.
//
// now() returns a 64-bit counter in nanoseconds, the native resolution of
// std::chrono::steady_clock on glibc, which is more than sufficient for
// microsecond timeslice budgeting. intervalSeconds() performs the same
// arithmetic as the original QPC code (count difference / frequency), so the
// budget-scaling behaviour is unchanged.
class HighResTimer {
public:
  // Current value of the monotonic counter, in nanoseconds.
  static std::int64_t now();

  // Seconds between two counter values returned by now().
  static double intervalSeconds(std::int64_t start, std::int64_t end);
};

}  // namespace application
}  // namespace WP_NAMESPACE
