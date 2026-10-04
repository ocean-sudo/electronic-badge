#pragma once

#include <cmath>
#include <cstdint>

// QMI8658 accelerations are in g. Waveshare's LVGL orientation example plus
// LVGL's counterclockwise compensation maps x+ to down and y- to left on the
// unrotated panel. Our image renderer uses clockwise centidegrees instead.
class GravityMotion {
 public:
  static constexpr uint32_t StaleMs = 500;
  uint16_t angle = 0;
  bool hasSample = false;
  bool valid = false;
  const char *error = "";
  float ax = 0, ay = 0, az = 0;

  void resume(uint32_t now) {
    lastSample_ = now;
    valid = filtered_ = false;
    error = "";
  }

  void suspend() {
    valid = filtered_ = false;
    // Keep an actual fault visible even after sampling has stopped.
  }

  void fault(const char *reason) {
    valid = filtered_ = false;
    error = reason;
  }

  void tick(uint32_t now) {
    if (now - lastSample_ >= StaleMs) fault("imu_stale");
  }

  void sample(float x, float y, float z, uint32_t now) {
    lastSample_ = now;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
      fault("imu_invalid_acceleration");
      return;
    }
    ax = x; ay = y; az = z;
    hasSample = true;
    const float magnitude2 = x * x + y * y + z * z;
    if (magnitude2 < 0.25f || magnitude2 > 2.25f) {
      fault("imu_invalid_acceleration");
      return;
    }
    error = "";
    const float planar2 = x * x + y * y;
    const float threshold = valid ? 0.15f : 0.22f;
    if (planar2 < threshold * threshold) {
      valid = filtered_ = false;
      return; // Face-up/down has no well-defined image bottom; hold its pose.
    }
    if (!filtered_) { fx_ = x; fy_ = y; filtered_ = true; }
    else { fx_ += 0.25f * (x - fx_); fy_ += 0.25f * (y - fy_); }
    if (fx_ * fx_ + fy_ * fy_ < 0.15f * 0.15f) {
      valid = false;
      return;
    }
    float degrees = std::atan2(-fy_, fx_) * (18000.0f / 3.14159265358979323846f);
    if (degrees < 0) degrees += 36000.0f;
    const uint16_t target = static_cast<uint16_t>(std::lround(degrees) % 36000);
    int difference = static_cast<int>(target) - angle;
    if (difference > 18000) difference -= 36000;
    if (difference < -18000) difference += 36000;
    // Half-degree deadband removes resting jitter without accumulating drift.
    if (!valid || difference >= 50 || difference <= -50) angle = target;
    valid = true;
  }

 private:
  uint32_t lastSample_ = 0;
  float fx_ = 0, fy_ = 0;
  bool filtered_ = false;
};
