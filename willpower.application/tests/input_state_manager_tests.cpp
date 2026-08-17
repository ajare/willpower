#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "willpower/application/InputStateManager.h"

namespace {

using wp::application::InputStateManager;

constexpr float Epsilon = 0.0001f;

void require(bool condition, std::string const& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireNear(float actual, float expected, std::string const& message) {
  require(std::abs(actual - expected) < Epsilon, message);
}

// The mouse position the platform layer reports first is wherever the cursor
// happens to sit, and must not be mistaken for motion.
void startsFromTheFirstPositionReported() {
  InputStateManager manager;
  float deltaX = -1.0f, deltaY = -1.0f;

  manager.setMousePosition(512.0f, 384.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 0.0f, "The first mouse position reported was read as motion.");
  requireNear(deltaY, 0.0f, "The first mouse position reported was read as motion.");
}

// Several motion events can arrive between two frames. All of the motion has
// to survive to the next process(), not just the last event's share of it.
void gathersMotionUntilProcessed() {
  InputStateManager manager;
  float deltaX = 0.0f, deltaY = 0.0f;

  manager.setMousePosition(0.0f, 0.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  manager.setMousePosition(3.2f, -1.0f);
  manager.setMousePosition(5.9f, -2.5f);
  manager.setMousePosition(7.8f, -4.0f);

  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 7.8f, "Motion from earlier in the frame was dropped.");
  requireNear(deltaY, -4.0f, "Motion from earlier in the frame was dropped.");
}

// Motion is reported once. A frame with no events is a frame with no motion.
void clearsMotionOnceProcessed() {
  InputStateManager manager;
  float deltaX = 0.0f, deltaY = 0.0f;

  manager.setMousePosition(0.0f, 0.0f);
  manager.setMousePosition(4.0f, 4.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 4.0f, "Motion was not reported.");

  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 0.0f, "A frame without motion repeated the previous frame's motion.");
  requireNear(deltaY, 0.0f, "A frame without motion repeated the previous frame's motion.");

  manager.setMousePosition(6.5f, 4.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 2.5f, "Motion is not measured from the position last processed.");
  requireNear(deltaY, 0.0f, "Motion is not measured from the position last processed.");
}

// Motion in opposite directions within one frame cancels out, rather than the
// last event standing in for the whole frame.
void gathersMotionInBothDirections() {
  InputStateManager manager;
  float deltaX = 0.0f, deltaY = 0.0f;

  manager.setMousePosition(0.0f, 0.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  manager.setMousePosition(10.0f, 0.0f);
  manager.setMousePosition(4.0f, 0.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 4.0f, "Motion that reversed within a frame did not add up.");
}

// Motion the state was not told about - while a GUI held the mouse, say -
// must not arrive in one burst once events resume.
void doesNotGatherMotionAcrossAResync() {
  InputStateManager manager;
  float deltaX = 0.0f, deltaY = 0.0f;

  manager.setMousePosition(10.0f, 10.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  manager.resyncMousePosition();

  // Wherever the mouse turned up, that position is a starting point again.
  manager.setMousePosition(900.0f, -400.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 0.0f, "The gap in motion events was read as motion.");
  requireNear(deltaY, 0.0f, "The gap in motion events was read as motion.");

  manager.setMousePosition(903.0f, -401.0f);
  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 3.0f, "Motion after a resync is not measured from the new position.");
  requireNear(deltaY, -1.0f, "Motion after a resync is not measured from the new position.");
}

// A resync breaks the position link, but motion already gathered is real and
// still owed to whoever asks for it next.
void keepsMotionGatheredBeforeAResync() {
  InputStateManager manager;
  float deltaX = 0.0f, deltaY = 0.0f;

  manager.setMousePosition(0.0f, 0.0f);
  manager.setMousePosition(5.0f, 0.0f);

  manager.resyncMousePosition();

  (void)manager.process(false, &deltaX, &deltaY);

  requireNear(deltaX, 5.0f, "Motion gathered before the resync was thrown away.");
}

// The position itself still tracks the latest event.
void reportsTheLatestPosition() {
  InputStateManager manager;
  float x = 0.0f, y = 0.0f;

  manager.setMousePosition(11.0f, 12.0f);
  manager.setMousePosition(13.0f, 14.0f);

  manager.mousePosition(&x, &y);

  requireNear(x, 13.0f, "The mouse position is not the latest one reported.");
  requireNear(y, 14.0f, "The mouse position is not the latest one reported.");
}

}  // namespace

int main() {
  try {
    startsFromTheFirstPositionReported();
    gathersMotionUntilProcessed();
    clearsMotionOnceProcessed();
    gathersMotionInBothDirections();
    doesNotGatherMotionAcrossAResync();
    keepsMotionGatheredBeforeAResync();
    reportsTheLatestPosition();

    std::cout << "Input state manager tests passed\n";
    return 0;
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
