#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "willpower/common/Platform.h"

namespace WP_NAMESPACE {

class WP_COMMON_API Timer {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using NowFunction = std::function<TimePoint()>;

private:
  NowFunction mNow;
  TimePoint mTimeStarted;
  Clock::duration mDuration = {};
  bool mPaused;

public:
  explicit Timer(NowFunction now = [] { return Clock::now(); });

  void restart();

  void resume();

  void pause();

  int64_t elapsedNanoseconds();

  static std::string nsToString(int64_t ns);

  std::string elapsedStr();
};

}  // namespace WP_NAMESPACE
