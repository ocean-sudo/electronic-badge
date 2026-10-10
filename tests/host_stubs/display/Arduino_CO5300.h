#pragma once
#include <cstdint>
#include <vector>

class Arduino_CO5300 {
 public:
  void setBrightness(uint8_t) {}
  void draw16bitRGBBitmap(int, int, const uint16_t *pixels, int width, int height) {
    presented.assign(pixels, pixels + width * height);
  }

  std::vector<uint16_t> presented;
};
