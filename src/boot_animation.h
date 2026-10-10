#pragma once

#include <stdint.h>

namespace badge_boot {
constexpr int Width = 466;
constexpr int Height = 466;
constexpr uint32_t FrameIntervalMs = 90;
constexpr int PatchRadius = 6;
constexpr int PatchExtent = PatchRadius * 2 + 1;
constexpr uint8_t PatchCount = 6;

namespace detail {
constexpr int Center = 232;
constexpr int PanelRadiusSquared = 233 * 233;
constexpr uint16_t Background = 0x0843;
constexpr int OrbitX[3] = {0, 88, -88};
constexpr int OrbitY[3] = {-94, 48, 48};

inline void dotCenter(uint8_t phase, unsigned dot, int16_t& x, int16_t& y) {
  int px = OrbitX[dot], py = OrbitY[dot];
  for (uint8_t turn = 0; turn < (phase + dot) % 4; ++turn) {
    const int prior = px;
    px = -py;
    py = prior;
  }
  x = static_cast<int16_t>(Center + px);
  y = static_cast<int16_t>(Center + py);
}

inline uint16_t dotColor(uint8_t phase, unsigned dot) {
  return dot == phase % 3 ? 0xFFFF : 0x5DDF;
}

inline void drawDot(uint16_t* pixels, uint16_t color) {
  for (int dy = -PatchRadius; dy <= PatchRadius; ++dy) {
    for (int dx = -PatchRadius; dx <= PatchRadius; ++dx) {
      pixels[(dy + PatchRadius) * PatchExtent + dx + PatchRadius] =
          dx * dx + dy * dy <= PatchRadius * PatchRadius ? color : Background;
    }
  }
}
}  // namespace detail

// Full first frame: fixed-size procedural RGB565 background and orbiting dots.
inline void render(uint16_t* pixels, uint8_t phase) {
  for (int y = 0; y < Height; ++y) {
    const int dy = y - detail::Center;
    for (int x = 0; x < Width; ++x) {
      const int dx = x - detail::Center;
      const int distance = dx * dx + dy * dy;
      uint16_t color = 0;
      if (distance <= detail::PanelRadiusSquared) {
        color = detail::Background;
        const int ring = distance - 132 * 132;
        if (ring >= -700 && ring <= 700) color = 0x1230;
      }
      pixels[y * Width + x] = color;
    }
  }
  for (unsigned dot = 0; dot < 3; ++dot) {
    int16_t x, y;
    detail::dotCenter(phase, dot, x, y);
    for (int dy = -PatchRadius; dy <= PatchRadius; ++dy) {
      for (int dx = -PatchRadius; dx <= PatchRadius; ++dx) {
        if (dx * dx + dy * dy <= PatchRadius * PatchRadius)
          pixels[(y + dy) * Width + x + dx] = detail::dotColor(phase, dot);
      }
    }
  }
}

class Animation {
 public:
  bool start(uint32_t now, uint16_t* pixels) {
    if (!pixels) return false;
    active_ = true;
    phase_ = previousPhase_ = 0;
    lastFrame_ = now;
    render(pixels, phase_);
    return true;
  }

  bool tick(uint32_t now) {
    if (!active_ || now - lastFrame_ < FrameIntervalMs) return false;
    lastFrame_ = now;
    previousPhase_ = phase_;
    ++phase_;
    return true;
  }

  // Produces one small replacement tile in caller-owned scratch, not a full frame.
  bool renderPatch(uint8_t index, uint16_t* scratch, int16_t& x, int16_t& y) const {
    if (index >= PatchCount || !scratch) return false;
    const unsigned dot = index % 3;
    const bool restore = index < 3;
    const uint8_t phase = restore ? previousPhase_ : phase_;
    detail::dotCenter(phase, dot, x, y);
    x -= PatchRadius;
    y -= PatchRadius;
    detail::drawDot(scratch, restore ? detail::Background : detail::dotColor(phase, dot));
    return true;
  }

  void stop() { active_ = false; }
  bool active() const { return active_; }
  uint8_t phase() const { return phase_; }

 private:
  bool active_ = false;
  uint8_t phase_ = 0;
  uint8_t previousPhase_ = 0;
  uint32_t lastFrame_ = 0;
};
}  // namespace badge_boot
