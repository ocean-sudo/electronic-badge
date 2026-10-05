#include "touch_gesture.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <iostream>

namespace {
using Action = GestureAction;

Action swipe(int x, int y, int endX, int endY, bool menu = false,
             int originButton = -1, uint32_t duration = 100) {
  TouchGesture gesture;
  gesture.begin(x, y, 100, menu, originButton, false);
  gesture.move(endX, endY);
  return gesture.release(100 + duration, originButton);
}

void photoDirectionsAndThresholds() {
  assert(swipe(232, 232, 152, 232) == Action::Next);
  assert(swipe(232, 232, 312, 232) == Action::Previous);
  assert(swipe(232, 232, 232, 152) == Action::OpenMenu);
  assert(swipe(232, 232, 232, 312) == Action::None);
  assert(swipe(232, 232, 153, 232) == Action::None);
  assert(swipe(232, 232, 311, 232) == Action::None);
  assert(swipe(232, 232, 232, 153) == Action::None);

  // Swipes are displacement gestures, not taps: neither the tap minimum nor
  // the hold deadline may turn a valid swipe into another action.
  assert(swipe(232, 232, 152, 232, false, -1, 1) == Action::Next);
  assert(swipe(232, 232, 312, 232, false, -1, 900) == Action::Previous);
  assert(swipe(232, 232, 232, 152, false, -1, 2000) == Action::OpenMenu);
  assert(swipe(232, 232, 232, 312, false, -1, 2000) == Action::None);

  TouchGesture gesture;
  gesture.begin(232, 232, 100, false, -1, false);
  gesture.move(312, 232);
  assert(gesture.hold(1100) == Action::None);
  assert(gesture.release(1200, -1) == Action::Previous);
  assert(gesture.release(1300, -1) == Action::None);
}

void dominanceAndDiagonals() {
  // At exactly 5:4 the major axis wins; one more minor-axis pixel rejects it.
  assert(swipe(232, 232, 132, 312) == Action::Next);
  assert(swipe(232, 232, 132, 313) == Action::None);
  assert(swipe(232, 232, 332, 152) == Action::Previous);
  assert(swipe(232, 232, 332, 151) == Action::None);
  assert(swipe(232, 232, 312, 132) == Action::OpenMenu);
  assert(swipe(232, 232, 313, 132) == Action::None);
  assert(swipe(232, 232, 312, 332, true) == Action::CloseMenu);
  assert(swipe(232, 232, 313, 332, true) == Action::None);
  assert(swipe(232, 232, 312, 312) == Action::None);
  assert(swipe(232, 232, 152, 152) == Action::None);
  assert(swipe(232, 232, 312, 312, false, -1, 1500) == Action::None);

  TouchGesture gesture;
  gesture.begin(232, 232, 100, true, 4, false);
  gesture.move(254, 254); // True distance exceeds 30 although neither axis does.
  assert(gesture.hold(1100) == Action::None);
  assert(gesture.release(1200, 4) == Action::None);
}

void menuDirectionsAndBackOrigin() {
  assert(swipe(232, 232, 232, 312, true) == Action::CloseMenu);
  assert(swipe(232, 232, 232, 311, true) == Action::None);
  assert(swipe(232, 232, 232, 152, true) == Action::None);
  assert(swipe(232, 232, 152, 232, true) == Action::None);
  assert(swipe(232, 232, 312, 232, true) == Action::None);

  assert(swipe(40, 110, 120, 110, true) == Action::Back);
  assert(swipe(40, 355, 120, 355, true) == Action::Back);
  assert(swipe(0, 232, 80, 232, true) == Action::Back);
  assert(swipe(40, 232, 119, 232, true) == Action::None);
  assert(swipe(41, 232, 121, 232, true) == Action::None);
  assert(swipe(40, 109, 120, 109, true) == Action::None);
  assert(swipe(40, 356, 120, 356, true) == Action::None);
  assert(swipe(40, 232, 120, 232, true, 2) == Action::None);
  assert(swipe(0, 110, 80, 110, true) == Action::None); // Outside circle.
  assert(swipe(40, 232, 140, 312, true) == Action::Back);
  assert(swipe(40, 232, 140, 313, true) == Action::None);

  // Close is a menu-wide gesture, including a contact beginning on a button.
  assert(swipe(232, 232, 232, 312, true, 2, 2000) == Action::CloseMenu);
  TouchGesture gesture;
  gesture.begin(40, 232, 100, true, -1, false);
  gesture.move(120, 232);
  assert(gesture.hold(1100) == Action::None);
  assert(gesture.release(1200, -1) == Action::Back);
}

void circularBoundary() {
  TouchGesture gesture;
  const int inside[][2] = {{0, 218}, {0, 232}, {465, 247}, {232, 0}, {232, 465}};
  for (const auto &point : inside) {
    gesture.begin(point[0], point[1], 0, false, -1, false);
    assert(gesture.release(35, -1) == Action::Next);
  }
  const int outside[][2] = {
      {0, 217}, {465, 248}, {0, 0}, {465, 465}, {-1, 232},
      {466, 232}, {232, -1}, {232, 466}};
  for (const auto &point : outside) {
    gesture.begin(point[0], point[1], 0, false, -1, false);
    assert(gesture.hold(900) == Action::None);
    gesture.move(232, 232);
    assert(gesture.release(1000, -1) == Action::None);
  }
  assert(swipe(232, 232, 0, 0) == Action::None);
  assert(swipe(232, 232, 466, 232) == Action::None);

  // Even a small departure from the circle must not become a tap after return.
  gesture.begin(0, 218, 0, true, 3, false);
  gesture.move(0, 217);
  gesture.move(0, 218);
  assert(gesture.release(100, 3) == Action::None);
}

void tapAndHoldBoundaries() {
  TouchGesture gesture;
  assert(gesture.hold(1000) == Action::None);
  assert(gesture.release(1000, -1) == Action::None);
  gesture.begin(232, 232, 100, false, -1, false);
  assert(gesture.release(134, -1) == Action::None);
  gesture.begin(232, 232, 100, false, -1, false);
  assert(gesture.release(135, -1) == Action::Next);
  gesture.begin(232, 232, 100, false, -1, false);
  assert(gesture.hold(999) == Action::None);
  assert(gesture.release(999, -1) == Action::Next);

  for (bool menu : {false, true}) {
    gesture.begin(232, 232, 100, menu, -1, false);
    assert(gesture.hold(999) == Action::None);
    assert(gesture.hold(1000) == Action::Sleep);
    assert(gesture.hold(2000) == Action::None);
    assert(gesture.release(2100, -1) == Action::None);
    gesture.begin(232, 232, 100, menu, -1, false);
    assert(gesture.release(1000, -1) == Action::Sleep);
  }
  gesture.begin(232, 232, 100, true, -1, false);
  assert(gesture.release(135, -1) == Action::None); // Menu background isn't Next.

  // The distance limit is inclusive: this 18/24 offset is exactly 30 pixels.
  gesture.begin(232, 232, 100, false, -1, false);
  gesture.move(250, 256);
  assert(gesture.release(135, -1) == Action::Next);
  gesture.begin(232, 232, 100, true, -1, false);
  gesture.move(250, 256);
  assert(gesture.hold(1000) == Action::Sleep);
  gesture.begin(232, 232, 100, false, -1, false);
  gesture.move(262, 232);
  assert(gesture.release(135, -1) == Action::Next);
  gesture.begin(232, 232, 100, false, -1, false);
  gesture.move(263, 232);
  assert(gesture.release(135, -1) == Action::None);
}

void capturedButtonsAndReversals() {
  TouchGesture gesture;
  gesture.begin(232, 232, 100, true, 7, false);
  assert(gesture.button() == 7);
  assert(gesture.release(135, 7) == Action::Button);
  assert(gesture.button() == 7); // Consumer dispatches the capture after release.
  assert(gesture.release(136, 7) == Action::None);
  gesture.begin(232, 232, 100, true, 7, false);
  assert(gesture.release(134, 7) == Action::None);
  gesture.begin(232, 232, 100, true, 7, false);
  assert(gesture.release(200, 8) == Action::None);
  gesture.begin(232, 232, 100, true, 7, false);
  assert(gesture.release(200, -1) == Action::None);
  gesture.begin(232, 232, 100, true, -1, false);
  assert(gesture.release(200, 7) == Action::None);

  // Stationary holds preserve Sleep even over a button; consuming the hold
  // prevents a second button action when that same contact is released.
  gesture.begin(232, 232, 100, true, 7, false);
  assert(gesture.hold(999) == Action::None);
  assert(gesture.hold(1000) == Action::Sleep);
  assert(gesture.hold(5000) == Action::None);
  assert(gesture.release(5100, 7) == Action::None);
  gesture.begin(232, 232, 100, true, 7, false);
  assert(gesture.release(999, 7) == Action::Button);
  gesture.begin(232, 232, 100, true, 7, false);
  assert(gesture.release(1000, 7) == Action::Sleep);

  gesture.begin(232, 232, 100, true, 7, false);
  gesture.move(250, 256);
  assert(gesture.release(200, 7) == Action::Button);
  for (bool menu : {false, true}) {
    const int captured = menu ? 7 : -1;
    gesture.begin(232, 232, 100, menu, captured, false);
    gesture.move(352, 232);
    gesture.move(232, 232); // Reversal cannot erase a previous excursion.
    assert(gesture.hold(1100) == Action::None);
    assert(gesture.release(1200, captured) == Action::None);
    gesture.begin(232, 232, 100, menu, captured, false);
    gesture.move(254, 254);
    gesture.move(232, 232); // Diagonal excursion also latches cancellation.
    assert(gesture.hold(1100) == Action::None);
    assert(gesture.release(1200, captured) == Action::None);
  }
  gesture.begin(232, 232, 100, true, 7, false);
  gesture.move(263, 232);
  gesture.move(233, 232);
  assert(gesture.release(200, 7) == Action::None);
}

void wakeCancelAndClockWrap() {
  TouchGesture gesture;
  for (bool menu : {false, true}) {
    gesture.begin(232, 232, 0, menu, 2, true);
    assert(gesture.hold(900) == Action::None);
    assert(gesture.release(1000, 2) == Action::None);
    gesture.begin(232, 232, 0, menu, -1, true);
    gesture.move(152, 232);
    assert(gesture.release(100, -1) == Action::None);
  }
  gesture.begin(232, 232, 0, false, -1, false);
  assert(gesture.release(35, -1) == Action::Next); // Only the wake contact is eaten.
  gesture.begin(232, 232, 0, true, 2, false);
  gesture.cancel();
  assert(gesture.button() == -1);
  gesture.move(232, 312);
  assert(gesture.hold(900) == Action::None);
  assert(gesture.release(1000, 2) == Action::None);
  gesture.begin(232, 232, 0, false, -1, false);
  gesture.move(232, 152);
  gesture.cancel();
  assert(gesture.release(100, -1) == Action::None);
  gesture.begin(232, 232, 0, true, 3, false);
  gesture.begin(232, 232, 10, false, -1, false); // New contact resets old capture.
  assert(gesture.button() == -1);
  assert(gesture.release(45, -1) == Action::Next);

  gesture.begin(232, 232, UINT32_MAX - 20, false, -1, false);
  assert(gesture.release(13, -1) == Action::None);
  gesture.begin(232, 232, UINT32_MAX - 20, false, -1, false);
  assert(gesture.release(14, -1) == Action::Next);
  gesture.begin(232, 232, UINT32_MAX - 100, true, -1, false);
  assert(gesture.hold(798) == Action::None);
  assert(gesture.hold(799) == Action::Sleep);
}
} // namespace

int main() {
  photoDirectionsAndThresholds();
  dominanceAndDiagonals();
  menuDirectionsAndBackOrigin();
  circularBoundary();
  tapAndHoldBoundaries();
  capturedButtonsAndReversals();
  wakeCancelAndClockWrap();
  std::cout << "touch gesture tests passed\n";
}
