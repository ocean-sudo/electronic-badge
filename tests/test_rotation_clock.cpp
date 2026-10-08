#include "rotation_clock.h"

#include <cassert>
#include <iostream>

int main() {
  badge_animation::RotationClock clock;
  constexpr uint16_t period = 24;
  clock.advance(5000, period * 1000U);
  assert(clock.angle(period) == 7500);

  // Panel refresh is rate-limited: elapsed time can be newer than its last frame.
  clock.recordPresented(clock.angle(period));
  const uint16_t inherited = clock.presentedAngle();
  clock.advance(80, period * 1000U);
  assert(clock.angle(period) > inherited);
  assert(clock.presentedAngle() == inherited);
  assert(inherited == 7500);  // A direct incoming frame inherits the displayed angle.

  // The transition does not reset the clock; rotation continues after it ends.
  const uint32_t beforeTransition = clock.elapsedMs();
  clock.advance(400, period * 1000U);
  const uint16_t transitionEnd = clock.angle(period);
  assert(clock.elapsedMs() == beforeTransition + 400);
  assert(transitionEnd > inherited);
  clock.recordPresented(transitionEnd);
  clock.advance(100, period * 1000U);
  assert(clock.angle(period) > clock.presentedAngle());
  assert(clock.elapsedMs() == beforeTransition + 500);

  clock.restore(23000, 3450);
  clock.advance(2000, period * 1000U);
  assert(clock.elapsedMs() == 1000);
  assert(clock.angle(period) == 1500);
  assert(clock.presentedAngle() == 3450);

  clock.reset();
  assert(clock.elapsedMs() == 0 && clock.presentedAngle() == 0);
  std::cout << "PASS: rotation clock pose/timing/wrap/restore/reset invariants\n";
}
