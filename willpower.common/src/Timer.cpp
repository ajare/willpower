#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

#include "willpower/common/Timer.h"

namespace WP_NAMESPACE {
using namespace std;

Timer::Timer(NowFunction now)
    : mNow(std::move(now)),
      mTimeStarted(mNow()),
      mPaused(false) {}

void Timer::restart() {
  mPaused = false;
  mDuration = {};
  mTimeStarted = mNow();
}

void Timer::resume() {
  if (!mPaused) {
    return;
  }

  mPaused = false;
  mTimeStarted = mNow();
}

void Timer::pause() {
  if (mPaused) {
    return;
  }

  mDuration += mNow() - mTimeStarted;
  mPaused = true;
}

int64_t Timer::elapsedNanoseconds() {
  auto duration = mDuration;
  if (!mPaused) {
    duration += mNow() - mTimeStarted;
  }

  return chrono::duration_cast<chrono::nanoseconds>(duration).count();
}

string Timer::nsToString(int64_t ns) {
  if (ns <= 0) {
    return "0 ns";
  }

  int nsecs_log10 = static_cast<int>(log10(ns));

  ostringstream os{};
  os.precision(static_cast<uint8_t>(2.0 - (nsecs_log10 % 3)));

  os << fixed;
  if (nsecs_log10 < 6)
    os << ns * 1.0e-3 << " us";
  else if (nsecs_log10 < 9)
    os << ns * 1.0e-6 << " ms";
  else
    os << ns * 1.0e-9 << " s";

  return os.str();
}

string Timer::elapsedStr() {
  return nsToString(elapsedNanoseconds());
}

}  // namespace WP_NAMESPACE
