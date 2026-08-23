#include "willpower/application/Scheduler.h"

#include "PlatformTimer.h"

#include "willpower/common/Exceptions.h"

using namespace std;

namespace WP_NAMESPACE {
namespace application {

Scheduler::Scheduler(int microseconds)
    : mMicroseconds(microseconds), mTotalMicrosecondsAllocated(0), mExecuting(false) {
}

void Scheduler::setMicrosecondsAllocated(int microseconds) {
  if (mExecuting) {
    throw Exception("Cannot set scheduler timeslice when executing.");
  }

  mMicroseconds = microseconds;
}

int Scheduler::getMicrosecondsAllocated() const {
  return mMicroseconds;
}

void Scheduler::addTask(SchedulerTask* task, int microseconds) {
  task->setMicrosecondsAllocated(microseconds);
  mTotalMicrosecondsAllocated += microseconds;

  CurrentTask ct;

  ct.task = task;
  ct.cumulativeMilliseconds = 0;

  if (!mTasks.empty()) {
    ct.cumulativeMilliseconds = mTasks.back().cumulativeMilliseconds + mTasks.back().task->getMicrosecondsAllocated();
  }

  mTasks.push_back(ct);
}

void Scheduler::execute(float frameTime) {
  mExecuting = true;

  int microsecondsLeft = mMicroseconds;
  float scaleRemaining = 1.0f;
  int64_t taskStartTime, taskEndTime;
  for (auto task : mTasks) {
    // Get task start time
    taskStartTime = HighResTimer::now();

    int taskMicroseconds = task.task->getMicrosecondsAllocated();
    if (taskMicroseconds > 0) {
      task.task->setMicrosecondsAllocated((int)(taskMicroseconds * scaleRemaining));
    }

    task.task->execute(frameTime);

    // Get task end time
    taskEndTime = HighResTimer::now();

    double interval = HighResTimer::intervalSeconds(taskStartTime, taskEndTime);
    int microsecondsPassed = (int)(interval * 1000000);
    microsecondsLeft -= microsecondsPassed;

    // If the total time allocated to the remaining tasks is greater than the time
    // left, then scale each down.
    int microsecondsRequired = mTotalMicrosecondsAllocated - task.cumulativeMilliseconds;
    if (microsecondsRequired > microsecondsLeft) {
      scaleRemaining *= (float)microsecondsLeft / (float)microsecondsRequired;
    }
  }

  mExecuting = false;
}

}  // namespace application
}  // namespace WP_NAMESPACE
