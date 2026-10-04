#include "gravity_motion.h"
#include "animation_renderer.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {
int distance(unsigned a, unsigned b) {
  int delta = static_cast<int>(a) - static_cast<int>(b);
  if (delta > 18000) delta -= 36000;
  if (delta < -18000) delta += 36000;
  return std::abs(delta);
}

void cardinalsAndPose() {
  constexpr float axes[][2] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}};
  constexpr int bottom[][2] = {{233, 332}, {133, 233}, {232, 133}, {332, 232}};
  constexpr int width = badge_animation::Width;
  std::vector<uint16_t> source(width * width), output(width * width);
  source[332 * width + 233] = 0xF800; // A marker at the canonical image bottom.
  for (unsigned i = 0; i < 4; ++i) {
    GravityMotion motion;
    assert(!motion.valid && !motion.hasSample && motion.angle == 0);
    motion.resume(10);
    motion.sample(axes[i][0], axes[i][1], 0, 60);
    assert(motion.valid && motion.hasSample && !*motion.error);
    assert(motion.angle == i * 9000);
    badge_animation::renderRotation(source.data(), output.data(), motion.angle);
    assert(output[bottom[i][1] * width + bottom[i][0]] == 0xF800);
    const auto pose = motion.angle;
    motion.suspend();
    assert(!motion.valid && motion.angle == pose);
    motion.resume(100);
    assert(!motion.valid && motion.angle == pose);
    motion.sample(axes[i][0], axes[i][1], 0, 150);
    assert(motion.valid && motion.angle == pose);
  }
}

void flatInvalidAndStale() {
  GravityMotion motion;
  motion.resume(0);
  motion.sample(0, 0, 1, 50);
  assert(!motion.valid && motion.angle == 0 && !*motion.error);
  motion.sample(0, -1, 0, 100);
  assert(motion.valid && motion.angle == 9000);
  motion.sample(0, -0.20f, 0.98f, 150); // Keep direction above the lower threshold.
  assert(motion.valid);
  motion.sample(0, -0.14f, 0.99f, 200);
  assert(!motion.valid && motion.angle == 9000 && !*motion.error);
  motion.sample(0, -0.20f, 0.98f, 250); // Hysteresis prevents flat-edge chatter.
  assert(!motion.valid && motion.angle == 9000);
  motion.sample(0, -0.23f, 0.97f, 300);
  assert(motion.valid && motion.angle == 9000);
  const float savedX = motion.ax, savedY = motion.ay, savedZ = motion.az;
  motion.sample(std::numeric_limits<float>::quiet_NaN(), 0, 1, 350);
  assert(!motion.valid && motion.angle == 9000 && std::strcmp(motion.error, "imu_invalid_acceleration") == 0);
  assert(motion.ax == savedX && motion.ay == savedY && motion.az == savedZ);
  for (float z : {0.0f, 0.1f, 2.0f, std::numeric_limits<float>::infinity()}) {
    motion.sample(0, 0, z, 400);
    assert(!motion.valid && motion.angle == 9000 && *motion.error);
  }
  motion.sample(-1, 0, 0, 500);
  assert(motion.valid && motion.angle == 18000 && !*motion.error);
  motion.tick(999);
  assert(motion.valid);
  motion.tick(1000);
  assert(!motion.valid && motion.angle == 18000 && std::strcmp(motion.error, "imu_stale") == 0);
  motion.sample(0, 1, 0, 1050);
  assert(motion.valid && motion.angle == 27000 && !*motion.error);
  motion.resume(UINT32_MAX - 100);
  motion.sample(1, 0, 0, UINT32_MAX - 50);
  motion.tick(448);
  assert(motion.valid);
  motion.tick(449);
  assert(!motion.valid); // Unsigned clock wrapping still expires at 500ms.
}

void wrapAndNoise() {
  GravityMotion motion;
  motion.resume(0);
  motion.sample(1, 0.02f, 0, 50);
  assert(motion.angle > 35000);
  for (unsigned i = 0; i < 30; ++i) {
    motion.sample(1, -0.02f, 0, 100 + i * 50);
    assert(motion.valid && distance(motion.angle, 0) < 120);
  }
  assert(motion.angle < 120); // Cross zero, never traverse 180 degrees.
  motion.resume(2000);
  motion.sample(1, 0, 0, 2050);
  for (unsigned i = 0; i < 100; ++i) {
    motion.sample(1, i % 2 ? 0.003f : -0.003f, 0, 2100 + i * 50);
    assert(motion.valid && motion.angle == 0); // Static pose needs no redraw.
  }
  for (unsigned i = 0; i < 30; ++i) motion.sample(0, -1, 0, 8000 + i * 50);
  assert(motion.valid && distance(motion.angle, 9000) < 55);
}

void rotatedSlideReference() {
  constexpr int width = badge_animation::Width;
  std::vector<uint16_t> old(width * width), incoming(width * width), posed(width * width),
      reference(width * width), guarded(width * width + 2, 0xA55A);
  for (int i = 0; i < width * width; ++i) {
    old[i] = static_cast<uint16_t>(i * 29U);
    incoming[i] = static_cast<uint16_t>(i * 997U);
  }
  const auto savedOld = old, savedIncoming = incoming;
  for (uint16_t angle : {0, 1, 4500, 9000, 18000, 27000, 35999}) {
    badge_animation::renderRotation(incoming.data(), posed.data(), angle);
    for (unsigned offset : {0U, 1U, 100U, 233U, 465U, 466U, 500U}) {
      badge_animation::renderSlide(old.data(), posed.data(), reference.data(), offset);
      badge_animation::renderSlide(old.data(), incoming.data(), guarded.data() + 1, offset, true, angle);
      assert(std::equal(reference.begin(), reference.end(), guarded.begin() + 1));
      assert(guarded.front() == 0xA55A && guarded.back() == 0xA55A);
      assert(old == savedOld && incoming == savedIncoming);
    }
  }
}
} // namespace

int main() {
  badge_animation::initialize();
  cardinalsAndPose();
  flatInvalidAndStale();
  wrapAndNoise();
  rotatedSlideReference();
  std::cout << "PASS: gravity cardinals/pose/slide/flat/invalid/stale/wrap/noise invariants\n";
}
