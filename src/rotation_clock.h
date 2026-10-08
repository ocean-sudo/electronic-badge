#pragma once

#include <stdint.h>

namespace badge_animation {

// Tracks continuous automatic rotation separately from the angle last
// submitted to the panel; image selection must inherit the latter.
class RotationClock {
 public:
  void reset() { elapsedMs_ = presentedAngle_ = 0; }
  void advance(uint32_t deltaMs, uint32_t periodMs) {
    elapsedMs_ = (elapsedMs_ + deltaMs) % periodMs;
  }
  uint32_t elapsedMs() const { return elapsedMs_; }
  uint16_t angle(uint16_t periodSeconds) const {
    return static_cast<uint16_t>(elapsedMs_ * 36U / periodSeconds);
  }
  uint16_t presentedAngle() const { return presentedAngle_; }
  void recordPresented(uint16_t angle) { presentedAngle_ = angle; }
  void restore(uint32_t elapsedMs, uint16_t presentedAngle) {
    elapsedMs_ = elapsedMs;
    presentedAngle_ = presentedAngle;
  }

 private:
  uint32_t elapsedMs_ = 0;
  uint16_t presentedAngle_ = 0;
};

}  // namespace badge_animation
