#include "willpower/application/SchedulerTask.h"

#include "PlatformTimer.h"

#include "willpower/common/Exceptions.h"
#include "willpower/common/WillpowerWalker.h"

using namespace std;

namespace WP_NAMESPACE {
namespace application {

SchedulerTask::SchedulerTask()
    : mMicroseconds(-1), mExecuting(false) {
}

void SchedulerTask::setMicrosecondsAllocated(int nanoseconds) {
  if (mExecuting) {
    throw Exception("Cannot set scheduler timeslice when executing.");
  }

  mMicroseconds = nanoseconds;
}

int SchedulerTask::getMicrosecondsAllocated() const {
  return mMicroseconds;
}

int SchedulerTask::getMicrosecondsSpent() const {
  ASSERT_TRACE(mExecuting && "Cannot get microseconds currently spent when not executing task.");

  // Get task end time
  int64_t curTime = HighResTimer::now();

  double interval = HighResTimer::intervalSeconds(mStartTime, curTime);
  return (int)(interval * 1000000);
}

void SchedulerTask::execute(float frameTime) {
  mExecuting = true;

  // Start timer
  mStartTime = HighResTimer::now();

  // Execution implementation
  executeImpl(frameTime);

  mExecuting = false;
}

}  // namespace application
}  // namespace WP_NAMESPACE
