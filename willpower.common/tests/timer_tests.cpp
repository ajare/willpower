#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include <willpower/common/Timer.h>

namespace {

class FakeClock {
public:
  using TimePoint = wp::Timer::TimePoint;

  TimePoint now() const {
    return mNow;
  }

  void advance(int64_t nanoseconds) {
    mNow += std::chrono::nanoseconds(nanoseconds);
  }

private:
  TimePoint mNow{};
};

void require(bool condition, char const* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

wp::Timer makeTimer(FakeClock const& clock) {
  return wp::Timer([&clock] { return clock.now(); });
}

void elapsedReadsAreMonotonicAndNonAccumulating() {
  FakeClock clock;
  auto timer = makeTimer(clock);

  require(timer.elapsedNanoseconds() == 0, "a new timer must have no elapsed time");

  clock.advance(10);
  require(timer.elapsedNanoseconds() == 10, "the first read must include the running interval");
  require(timer.elapsedNanoseconds() == 10,
          "repeated reads must not accumulate the same running interval");

  clock.advance(5);
  require(timer.elapsedNanoseconds() == 15, "elapsed time must advance with the clock");
  require(timer.elapsedNanoseconds() == 15, "elapsed reads must remain monotonic");
}

void pauseAndResumeAccumulateEachIntervalOnce() {
  FakeClock clock;
  auto timer = makeTimer(clock);

  clock.advance(10);
  require(timer.elapsedNanoseconds() == 10,
          "an elapsed read must not consume the interval before pause");
  timer.pause();
  require(timer.elapsedNanoseconds() == 10, "pause must record the outstanding interval");

  clock.advance(40);
  timer.pause();
  require(timer.elapsedNanoseconds() == 10, "a repeated pause must not add another interval");

  timer.resume();
  clock.advance(5);
  timer.resume();
  clock.advance(5);
  require(timer.elapsedNanoseconds() == 20,
          "resume must start one new interval and remain idempotent while running");

  timer.pause();
  require(timer.elapsedNanoseconds() == 20, "pause must record the resumed interval once");
  clock.advance(100);
  require(timer.elapsedNanoseconds() == 20, "a paused timer must not advance");
}

void restartClearsElapsedTime() {
  FakeClock clock;
  auto timer = makeTimer(clock);

  clock.advance(10);
  timer.pause();
  timer.restart();
  require(timer.elapsedNanoseconds() == 0, "restart must clear prior elapsed time");

  clock.advance(5);
  require(timer.elapsedNanoseconds() == 5, "restart must begin a new interval");
}

void nonPositiveDurationsHaveDefinedFormatting() {
  require(wp::Timer::nsToString(0) == "0 ns", "zero duration must format as zero nanoseconds");
  require(wp::Timer::nsToString(-1) == "0 ns",
          "negative duration must format as zero nanoseconds");
}

}  // namespace

int main() {
  try {
    elapsedReadsAreMonotonicAndNonAccumulating();
    pauseAndResumeAccumulateEachIntervalOnce();
    restartClearsElapsedTime();
    nonPositiveDurationsHaveDefinedFormatting();
    std::cout << "Timer tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
