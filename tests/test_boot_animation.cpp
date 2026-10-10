#include "boot_animation.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
  constexpr size_t pixels = static_cast<size_t>(badge_boot::Width) * badge_boot::Height;
  std::vector<uint16_t> screen(pixels), reference(pixels), scratch(badge_boot::PatchExtent * badge_boot::PatchExtent);
  badge_boot::Animation animation;
  assert(!animation.start(100, nullptr));
  assert(!animation.active());
  assert(animation.start(100, screen.data()));
  assert(animation.active() && animation.phase() == 0);
  assert(!animation.tick(189));
  assert(animation.phase() == 0);
  assert(animation.tick(190));
  assert(animation.phase() == 1);
  badge_boot::render(reference.data(), animation.phase());
  for (uint8_t patch = 0; patch < badge_boot::PatchCount; ++patch) {
    int16_t x, y;
    assert(animation.renderPatch(patch, scratch.data(), x, y));
    assert(x >= 0 && y >= 0);
    assert(x + badge_boot::PatchExtent <= badge_boot::Width);
    assert(y + badge_boot::PatchExtent <= badge_boot::Height);
    for (int row = 0; row < badge_boot::PatchExtent; ++row)
      for (int col = 0; col < badge_boot::PatchExtent; ++col)
        screen[static_cast<size_t>(y + row) * badge_boot::Width + x + col] =
            scratch[static_cast<size_t>(row) * badge_boot::PatchExtent + col];
  }
  assert(screen == reference);
  const auto handedOff = screen;
  const uint8_t handedOffPhase = animation.phase();
  animation.stop();
  assert(!animation.active());
  assert(!animation.tick(1000));
  assert(animation.phase() == handedOffPhase);
  assert(screen == handedOff);
  std::cout << "PASS: boot progression, six local patches, and stop/handoff freeze\n";
}
