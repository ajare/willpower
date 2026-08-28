#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "willpower/application/Scheduler.h"
#include "willpower/common/Exceptions.h"

namespace {

using wp::application::Scheduler;
using wp::application::SchedulerTask;

// These tests are timing independent by construction: HighResTimer reads the
// real steady_clock, so instead of asserting exact durations the scenarios
// use budgets that are orders of magnitude larger (or smaller) than the work
// the tasks actually do. Every assertion therefore holds whatever the
// machine's actual timing turns out to be.

void require(bool condition, std::string const& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireNear(float actual, float expected, std::string const& message) {
  require(std::abs(actual - expected) < 0.0001f, message);
}

std::string formatRunLog(std::vector<int> const& runLog) {
  std::string text;
  for (size_t i = 0; i < runLog.size(); ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += std::to_string(runLog[i]);
  }
  return text.empty() ? "<none>" : text;
}

bool ranInOrder(std::vector<int> const& runLog, std::vector<int> const& expected) {
  return runLog == expected;
}

// A SchedulerTask that records, by id, when it runs and with which frame time,
// so tests can check ordering without any timing assumptions. The optional
// hook runs inside executeImpl, i.e. while the task counts as executing.
class RecordingTask : public SchedulerTask {
public:
  RecordingTask(int id, std::vector<int>* runLog)
      : mId(id), mRunLog(runLog) {
  }

  void setDuringExecuteHook(std::function<void()> hook) {
    mHook = std::move(hook);
  }

  float lastFrameTime() const {
    return mLastFrameTime;
  }

  // Protected in SchedulerTask; exposed for the tests.
  int microsecondsSpent() const {
    return getMicrosecondsSpent();
  }

protected:
  void executeImpl(float frameTime) override {
    mRunLog->push_back(mId);
    mLastFrameTime = frameTime;
    if (mHook) {
      mHook();
    }
  }

private:
  int mId;
  std::vector<int>* mRunLog;
  float mLastFrameTime = 0.0f;
  std::function<void()> mHook;
};

// Tasks run in the order they were added, the order holds across repeated
// timeslices, and each task receives its timeslice's frame time.
void executesTasksInRegistrationOrderAcrossTimeslices() {
  std::vector<int> runLog;

  RecordingTask first(1, &runLog);
  RecordingTask second(2, &runLog);
  RecordingTask third(3, &runLog);

  // A 10 second timeslice; the tasks claim 1 second each, far under budget,
  // so nothing here depends on the machine's speed.
  Scheduler scheduler(10'000'000);
  scheduler.addTask(&first, 1'000'000);
  scheduler.addTask(&second, 1'000'000);
  scheduler.addTask(&third, 1'000'000);

  scheduler.execute(0.016f);
  require(ranInOrder(runLog, {1, 2, 3}), "The first timeslice did not run the tasks in registration order: " + formatRunLog(runLog));

  scheduler.execute(0.033f);
  require(ranInOrder(runLog, {1, 2, 3, 1, 2, 3}), "The second timeslice did not repeat the registration order: " + formatRunLog(runLog));

  requireNear(first.lastFrameTime(), 0.033f, "Tasks do not receive the frame time of their timeslice.");
  requireNear(second.lastFrameTime(), 0.033f, "Tasks do not receive the frame time of their timeslice.");
  requireNear(third.lastFrameTime(), 0.033f, "Tasks do not receive the frame time of their timeslice.");
}

// A task registered with a zero budget is still run, in order, between its
// neighbours.
void runsZeroBudgetTasksInOrder() {
  std::vector<int> runLog;

  RecordingTask first(1, &runLog);
  RecordingTask freeRider(2, &runLog);
  RecordingTask third(3, &runLog);

  Scheduler scheduler(10'000'000);
  scheduler.addTask(&first, 1'000'000);
  scheduler.addTask(&freeRider, 0);
  scheduler.addTask(&third, 1'000'000);

  scheduler.execute(0.016f);

  require(ranInOrder(runLog, {1, 2, 3}), "A zero-budget task was skipped out of order: " + formatRunLog(runLog));
  require(freeRider.getMicrosecondsAllocated() == 0, "A zero-budget task's allocation was changed.");
}

// When the timeslice is smaller than the combined claims, a task that runs
// after the budget is exhausted is scaled back to fit what is left, while the
// first task still runs at its full allocation.
void scalesBackTasksThatOvershootTheBudget() {
  std::vector<int> runLog;

  RecordingTask first(1, &runLog);
  RecordingTask second(2, &runLog);

  // A 1 second timeslice; both tasks claim 1 second each.
  Scheduler scheduler(1'000'000);
  scheduler.addTask(&first, 1'000'000);
  scheduler.addTask(&second, 1'000'000);

  scheduler.execute(0.016f);

  require(first.getMicrosecondsAllocated() == 1'000'000, "The first task, which ran with budget to spare, was scaled.");
  require(second.getMicrosecondsAllocated() <= 500'000, "A task over its budget was not scaled back to fit the remaining time.");
  require(second.getMicrosecondsAllocated() >= 0, "Scaling produced a negative allocation.");
}

// Scaling compounds down the task list: every task after the budget runs out
// gets a smaller share of its claim than the one before it.
void scalesRemainingTasksProgressively() {
  std::vector<int> runLog;

  RecordingTask first(1, &runLog);
  RecordingTask second(2, &runLog);
  RecordingTask third(3, &runLog);

  // A 1 second timeslice; all three tasks claim 1 second each.
  Scheduler scheduler(1'000'000);
  scheduler.addTask(&first, 1'000'000);
  scheduler.addTask(&second, 1'000'000);
  scheduler.addTask(&third, 1'000'000);

  scheduler.execute(0.016f);

  require(ranInOrder(runLog, {1, 2, 3}), "Scaling changed the task order: " + formatRunLog(runLog));
  require(first.getMicrosecondsAllocated() == 1'000'000, "The first task, which ran with budget to spare, was scaled.");

  // After the first task, at most 1 second is left of a 3 second claim, so
  // the second task is scaled to about a third of its claim.
  require(second.getMicrosecondsAllocated() <= 333'334, "The second task was not scaled down to the remaining budget.");
  require(second.getMicrosecondsAllocated() >= 0, "Scaling produced a negative allocation.");

  // After the second task, even less is left of the remaining 2 second
  // claim, so the third task's share is smaller again.
  require(third.getMicrosecondsAllocated() < second.getMicrosecondsAllocated(), "The third task was not scaled further down than the second.");
  require(third.getMicrosecondsAllocated() >= 0, "Scaling produced a negative allocation.");
}

// Tasks that fit inside the timeslice are not touched by the scaler.
void leavesUnderBudgetTasksUnaffected() {
  std::vector<int> runLog;

  RecordingTask first(1, &runLog);
  RecordingTask second(2, &runLog);

  // A 10 second timeslice; the tasks claim 100 ms each.
  Scheduler scheduler(10'000'000);
  scheduler.addTask(&first, 100'000);
  scheduler.addTask(&second, 100'000);

  scheduler.execute(0.016f);

  require(first.getMicrosecondsAllocated() == 100'000, "An under-budget task's allocation was changed.");
  require(second.getMicrosecondsAllocated() == 100'000, "An under-budget task's allocation was changed.");
}

// getMicrosecondsSpent() is only meaningful while the task is executing;
// inside executeImpl it must report a non-negative amount of time.
void reportsMicrosecondsSpentWhileExecuting() {
  std::vector<int> runLog;
  int spentWhileExecuting = -1;

  RecordingTask task(1, &runLog);
  task.setDuringExecuteHook([&task, &spentWhileExecuting]() {
    spentWhileExecuting = task.microsecondsSpent();
  });

  Scheduler scheduler(10'000'000);
  scheduler.addTask(&task, 1'000'000);

  scheduler.execute(0.016f);

  require(spentWhileExecuting >= 0, "getMicrosecondsSpent() reported negative time inside executeImpl.");
}

// Changing the timeslice in the middle of one throws, is not applied, and the
// tasks after the offending one do not run.
void rejectsTimesliceChangesWhileExecuting() {
  std::vector<int> runLog;

  RecordingTask first(1, &runLog);
  RecordingTask second(2, &runLog);

  Scheduler scheduler(1'000'000);
  scheduler.addTask(&first, 500'000);
  scheduler.addTask(&second, 500'000);

  first.setDuringExecuteHook([&scheduler]() {
    scheduler.setMicrosecondsAllocated(10);
  });

  bool threw = false;
  try {
    scheduler.execute(0.016f);
  } catch (wp::Exception const&) {
    threw = true;
  }

  require(threw, "Changing the timeslice while executing must throw.");
  require(scheduler.getMicrosecondsAllocated() == 1'000'000, "A rejected timeslice change was applied.");
  require(ranInOrder(runLog, {1}), "Tasks after the exception must not run: " + formatRunLog(runLog));
}

}  // namespace

int main() {
  try {
    executesTasksInRegistrationOrderAcrossTimeslices();
    runsZeroBudgetTasksInOrder();
    scalesBackTasksThatOvershootTheBudget();
    scalesRemainingTasksProgressively();
    leavesUnderBudgetTasksUnaffected();
    reportsMicrosecondsSpentWhileExecuting();
    rejectsTimesliceChangesWhileExecuting();

    std::cout << "Scheduler tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
