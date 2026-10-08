#include "animation_renderer.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
constexpr int W = badge_animation::Width;
constexpr int H = badge_animation::Height;
constexpr int N = W * H;

bool visible(int x, int y) {
  const double dx = x - (W - 1) / 2.0;
  const double dy = y - (H - 1) / 2.0;
  return dx * dx + dy * dy <= (W / 2.0) * (W / 2.0);
}

void checkUniformAndCardinals() {
  std::vector<uint16_t> source(N), guarded(N + 2, 0xA55A);
  auto *output = guarded.data() + 1;
  const uint16_t colors[] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x4B69};
  const uint16_t angles[] = {0, 1, 4499, 4500, 8999, 9000, 9001, 17999, 18000, 18001, 26999, 27000, 27001, 35999};
  for (uint16_t color : colors) {
    std::fill(source.begin(), source.end(), color);
    for (uint16_t angle : angles) {
      badge_animation::renderRotation(source.data(), output, angle);
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
          assert(output[y * W + x] == (visible(x, y) ? color : 0));
      assert(guarded.front() == 0xA55A && guarded.back() == 0xA55A);
    }
  }
  for (int i = 0; i < N; ++i) source[i] = static_cast<uint16_t>(i * 997U);
  for (unsigned quarter = 0; quarter < 4; ++quarter) {
    badge_animation::renderRotation(source.data(), output, quarter * 9000);
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        if (!visible(x, y)) { assert(output[y * W + x] == 0); continue; }
        const int sx = quarter == 0 ? x : quarter == 1 ? y : quarter == 2 ? W - 1 - x : W - 1 - y;
        const int sy = quarter == 0 ? y : quarter == 1 ? H - 1 - x : quarter == 2 ? H - 1 - y : x;
        assert(output[y * W + x] == source[sy * W + sx]);
      }
    }
  }
}

void checkBilinearChannels() {
  std::vector<uint16_t> source(N, 0), output(N);
  source[231 * W + 232] = 0xF800;
  source[231 * W + 233] = 0x001F;
  source[232 * W + 232] = 0x07E0;
  source[232 * W + 233] = 0xFFFF;
  const auto original = source;
  badge_animation::renderRotation(source.data(), output.data(), 4500);
  // Destination (233,232) maps to (232.5,231.7929...), Q5 weights16/25.
  // Independent channel averages: red16, green49, blue16. Packed565 averaging
  // or premature rounding changes this visible color.
  assert(output[232 * W + 233] == static_cast<uint16_t>((16 << 11) | (49 << 5) | 16));
  assert(source == original);
}


void checkSlideAndShift() {
  std::vector<uint16_t> oldImage(N), newImage(N), output(N);
  for (int i = 0; i < N; ++i) {
    oldImage[i] = static_cast<uint16_t>((i * 29U) | 1U);
    newImage[i] = static_cast<uint16_t>((i * 101U) | 0x8000U);
  }
  const auto savedOld = oldImage, savedNew = newImage;
  for (unsigned offset = 0; offset <= W; ++offset) {
    badge_animation::renderSlide(oldImage.data(), newImage.data(), output.data(), offset);
    for (int y : {0, 13, 232, 233, 451, 465}) {
      for (int x = 0; x < W; ++x) {
        const unsigned seam = W - offset;
        const uint16_t expected = !visible(x, y) ? 0 : static_cast<unsigned>(x) < seam
            ? oldImage[y * W + x + offset] : newImage[y * W + x - seam];
        assert(output[y * W + x] == expected);
      }
    }
  }
  std::vector<uint16_t> rotated(N);
  badge_animation::renderRotation(newImage.data(), rotated.data(), 4500);
  badge_animation::renderSlide(oldImage.data(), newImage.data(), output.data(), W, true, 4500);
  assert(output == rotated);
  const unsigned middleOffset = 350;
  const int incomingX = 280, incomingY = 232;
  badge_animation::renderSlide(oldImage.data(), newImage.data(), output.data(),
                               middleOffset, true, 4500);
  assert(output[incomingY * W + (W - middleOffset + incomingX)] ==
         rotated[incomingY * W + incomingX]);
  assert(output[232 * W + 100] == oldImage[232 * W + 450]);

  for (int dx : {-465, -2, 0, 2, 465}) {
    for (int dy : {-465, -1, 0, 1, 465}) {
      badge_animation::renderShift(oldImage.data(), output.data(), dx, dy);
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          const int sx = x - dx, sy = y - dy;
          const uint16_t expected = visible(x, y) && sx >= 0 && sx < W && sy >= 0 && sy < H
              ? oldImage[sy * W + sx] : 0;
          assert(output[y * W + x] == expected);
        }
      }
    }
  }
  for (int displacement : {INT_MIN, INT_MAX, -466, 466}) {
    badge_animation::renderShift(oldImage.data(), output.data(), displacement, displacement);
    for (auto pixel : output) assert(pixel == 0);
  }
  assert(oldImage == savedOld && newImage == savedNew);
}
uint16_t brighter(uint16_t pixel) {
  const uint32_t red = (pixel >> 11) & 31U;
  const uint32_t green = (pixel >> 5) & 63U;
  const uint32_t blue = pixel & 31U;
  return static_cast<uint16_t>(((red + ((31U - red) >> 2)) << 11) |
                               ((green + ((63U - green) >> 2)) << 5) |
                               (blue + ((31U - blue) >> 2)));
}

uint16_t dimmer(uint16_t pixel) {
  const uint32_t red = (pixel >> 11) & 31U;
  const uint32_t green = (pixel >> 5) & 63U;
  const uint32_t blue = pixel & 31U;
  return static_cast<uint16_t>(((red - (red >> 2)) << 11) |
                               ((green - (green >> 2)) << 5) |
                               (blue - (blue >> 2)));
}

void checkRipple() {
  std::vector<uint16_t> oldImage(N, 0x4208), newImage(N, 0x1A35);
  const auto oldOriginal = oldImage;
  const auto newOriginal = newImage;
  std::vector<uint16_t> guarded(N + 2, 0xA55A);
  uint16_t* output = guarded.data() + 1;

  badge_animation::renderRipple(oldImage.data(), newImage.data(), output, 0);
  assert(guarded.front() == 0xA55A && guarded.back() == 0xA55A);
  assert(output[232 * W + 232] == 0x4208);
  assert(output[0] == 0);  // Circular panel exterior remains black.
  assert(output[232 * W + 0] == 0x4208);

  badge_animation::renderRipple(oldImage.data(), newImage.data(), output, 128);
  assert(output[232 * W + 232] == 0x1A35);  // Center has been revealed.
  assert(output[232 * W + 348] == brighter(0x1A35));  // Bright inner band.
  assert(output[232 * W + 350] == dimmer(0x4208));   // Dark outer band.
  assert(output[232 * W + 400] == 0x4208);  // Beyond the wavefront.
  assert(output[0] == 0);
  assert(guarded.front() == 0xA55A && guarded.back() == 0xA55A);

  badge_animation::renderRipple(oldImage.data(), newImage.data(), output, 255);
  assert(output[232 * W + 232] == 0x1A35);
  assert(output[232 * W + 0] == 0x1A35);
  assert(output[0] == 0);
  assert(guarded.front() == 0xA55A && guarded.back() == 0xA55A);
  assert(oldImage == oldOriginal && newImage == newOriginal);
  std::vector<uint16_t> patterned(N), rotated(N);
  for (int i = 0; i < N; ++i) patterned[i] = static_cast<uint16_t>(i * 997U);
  const auto patternedOriginal = patterned;
  badge_animation::renderRotation(patterned.data(), rotated.data(), 4500);
  badge_animation::renderRipple(oldImage.data(), patterned.data(), output, 255, true, 4500);
  assert(std::equal(output, output + N, rotated.begin()));
  assert(guarded.front() == 0xA55A && guarded.back() == 0xA55A);
  badge_animation::renderRipple(oldImage.data(), patterned.data(), output, 128, true, 4500);
  assert(output[232 * W + 260] == rotated[232 * W + 260]);
  assert(output[232 * W + 400] == oldImage[232 * W + 400]);
  assert(output[0] == 0);
  assert(guarded.front() == 0xA55A && guarded.back() == 0xA55A);
  assert(patterned == patternedOriginal && oldImage == oldOriginal);
}
} // namespace

int main() {
  badge_animation::initialize();
  checkUniformAndCardinals();
  checkBilinearChannels();
  checkRipple();
  checkSlideAndShift();
  std::cout << "PASS: rotation/cardinal/channel/edge/source/slide/ripple/shift invariants\n";
}
