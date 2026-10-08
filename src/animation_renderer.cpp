#include "animation_renderer.h"

// Arduino's size-oriented defaults are costly in the per-pixel affine loop.
// Optimize only this renderer; leave the framework and vendor drivers alone.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")
#endif

#include <cmath>
#include <cstring>

namespace badge_animation {
namespace {

uint16_t rowStart[Height];
uint16_t rowEnd[Height];  // Exclusive.
bool initialized = false;

constexpr int32_t One = 65536;
constexpr int32_t Center = (Width - 1) * (One / 2);
constexpr int32_t MaxCoordinate = (Width - 1) * One;
constexpr float RadiansPerCentiDegree =
    3.14159265358979323846f / 18000.0f;

void clearOutside(uint16_t* row, int begin, int end) {
  std::memset(row, 0, begin * sizeof(uint16_t));
  std::memset(row + end, 0, (Width - end) * sizeof(uint16_t));
}

int32_t coefficient(float value) {
  const float scaled = value * One;
  return static_cast<int32_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

uint32_t redBlue(uint16_t pixel) {
  // Put red at bit 16 so the 10-bit weighted blue sum cannot carry into red.
  return ((static_cast<uint32_t>(pixel) & 0xF800U) << 5) |
         (pixel & 0x001FU);
}

uint16_t interpolate(const uint16_t* source, int32_t x, int32_t y) {
  // Circle-edge coordinates can fall up to half a texel outside the square.
  // Replicate its edge texels rather than reading outside or adding a dark rim.
  if (x < 0) x = 0;
  if (x > MaxCoordinate) x = MaxCoordinate;
  if (y < 0) y = 0;
  if (y > MaxCoordinate) y = MaxCoordinate;

  // Round the Q16 coordinates to Q5, including carries into the integer part.
  const uint32_t x5 = (static_cast<uint32_t>(x) + 1024U) >> 11;
  const uint32_t y5 = (static_cast<uint32_t>(y) + 1024U) >> 11;
  const unsigned sx = x5 >> 5;
  const unsigned sy = y5 >> 5;
  const unsigned wx = x5 & 31U;
  const unsigned wy = y5 & 31U;
  const unsigned ix = 32U - wx;
  const unsigned iy = 32U - wy;
  const unsigned right = sx < static_cast<unsigned>(Width - 1) ? 1U : 0U;
  const unsigned below = sy < static_cast<unsigned>(Height - 1) ? Width : 0U;
  const uint16_t* pixel = source + sy * Width + sx;
  const uint16_t p00 = pixel[0];
  const uint16_t p10 = pixel[right];
  const uint16_t p01 = pixel[below];
  const uint16_t p11 = pixel[below + right];

  // Keep both interpolation stages unrounded, then round each channel once.
  // Weights sum to 1024; all packed sums fit uint32_t without channel carries.
  const uint32_t rbTop = redBlue(p00) * ix + redBlue(p10) * wx;
  const uint32_t rbBottom = redBlue(p01) * ix + redBlue(p11) * wx;
  const uint32_t rb = (rbTop * iy + rbBottom * wy + 0x02000200U) >> 10;
  const uint32_t gTop = ((p00 >> 5) & 63U) * ix + ((p10 >> 5) & 63U) * wx;
  const uint32_t gBottom = ((p01 >> 5) & 63U) * ix + ((p11 >> 5) & 63U) * wx;
  const uint32_t green = (gTop * iy + gBottom * wy + 512U) >> 10;
  return static_cast<uint16_t>(((rb >> 5) & 0xF800U) |
                               (green << 5) | (rb & 0x001FU));
}

void rotationCoefficients(unsigned angle, int32_t& cosine, int32_t& sine) {
  switch (angle) {
    case 0U: cosine = One; sine = 0; break;
    case 9000U: cosine = 0; sine = One; break;
    case 18000U: cosine = -One; sine = 0; break;
    case 27000U: cosine = 0; sine = -One; break;
    default:
      const float radians = angle * RadiansPerCentiDegree;
      cosine = coefficient(std::cos(radians));
      sine = coefficient(std::sin(radians));
      break;
  }
}

}  // namespace

void initialize() {
  if (initialized) return;

  // Doubled pixel-center coordinates avoid square roots and fractional radii.
  const int radiusSquared = Width * Width;
  for (int y = 0; y < Height; ++y) {
    const int doubledY = 2 * y - (Height - 1);
    const int available = radiusSquared - doubledY * doubledY;
    int begin = 0;
    while (begin < Width / 2) {
      const int doubledX = 2 * begin - (Width - 1);
      if (doubledX * doubledX <= available) break;
      ++begin;
    }
    rowStart[y] = static_cast<uint16_t>(begin);
    rowEnd[y] = static_cast<uint16_t>(Width - begin);
  }
  initialized = true;
}

void renderRotation(const uint16_t* source, uint16_t* output,
                    uint16_t angleCentiDegrees) {
  const unsigned angle = angleCentiDegrees % 36000U;
  if (angle == 0U) {
    renderShift(source, output, 0, 0);
    return;
  }

  int32_t cosine;
  int32_t sine;
  const bool exactQuarterTurn = angle % 9000U == 0U;
  rotationCoefficients(angle, cosine, sine);

  for (int y = 0; y < Height; ++y) {
    const int begin = rowStart[y];
    const int end = rowEnd[y];
    uint16_t* row = output + y * Width;
    clearOutside(row, begin, end);
    const int doubledX = 2 * begin - (Width - 1);
    const int doubledY = 2 * y - (Height - 1);
    // Inverse clockwise transform, using Q16 coefficients and half-pixel center.
    int32_t sourceX = Center + (cosine * doubledX + sine * doubledY) / 2;
    int32_t sourceY = Center + (-sine * doubledX + cosine * doubledY) / 2;
    if (exactQuarterTurn) {
      // Cardinal mappings are exact integer texels and need no interpolation.
      for (int x = begin; x < end; ++x) {
        row[x] = source[(sourceY / One) * Width + sourceX / One];
        sourceX += cosine;
        sourceY -= sine;
      }
    } else {
      for (int x = begin; x < end; ++x) {
        row[x] = interpolate(source, sourceX, sourceY);
        sourceX += cosine;
        sourceY -= sine;
      }
    }
  }
}

void renderShift(const uint16_t* source, uint16_t* output, int dx, int dy) {
  // Reject non-overlapping translations before subtracting arbitrary int inputs.
  if (dx <= -Width || dx >= Width || dy <= -Height || dy >= Height) {
    std::memset(output, 0, Width * Height * sizeof(uint16_t));
    return;
  }

  for (int y = 0; y < Height; ++y) {
    uint16_t* row = output + y * Width;
    const int sourceY = y - dy;
    if (sourceY < 0 || sourceY >= Height) {
      std::memset(row, 0, Width * sizeof(uint16_t));
      continue;
    }
    const int begin = rowStart[y] > dx ? rowStart[y] : dx;
    const int sourceEnd = Width + dx;
    const int end = rowEnd[y] < sourceEnd ? rowEnd[y] : sourceEnd;
    if (begin >= end) {
      std::memset(row, 0, Width * sizeof(uint16_t));
      continue;
    }
    clearOutside(row, begin, end);
    std::memcpy(row + begin, source + sourceY * Width + begin - dx,
                (end - begin) * sizeof(uint16_t));
  }
}

void renderSlide(const uint16_t* oldImage, const uint16_t* newImage,
                 uint16_t* output, unsigned offset,
                 bool rotateIncoming, uint16_t incomingAngle) {
  if (offset > static_cast<unsigned>(Width)) offset = Width;
  const int shift = static_cast<int>(offset);
  const int seam = Width - shift;
  const unsigned angle = incomingAngle % 36000U;
  int32_t cosine = One, sine = 0;
  if (rotateIncoming) rotationCoefficients(angle, cosine, sine);
  const bool exactQuarterTurn = angle % 9000U == 0U;
  for (int y = 0; y < Height; ++y) {
    const int begin = rowStart[y];
    const int end = rowEnd[y];
    uint16_t* row = output + y * Width;
    clearOutside(row, begin, end);
    const int oldEnd = end < seam ? end : seam;
    if (begin < oldEnd) {
      std::memcpy(row + begin, oldImage + y * Width + begin + shift,
                  (oldEnd - begin) * sizeof(uint16_t));
    }
    const int newBegin = begin > seam ? begin : seam;
    if (newBegin < end) {
      if (!rotateIncoming) {
        std::memcpy(row + newBegin, newImage + y * Width + newBegin - seam,
                    (end - newBegin) * sizeof(uint16_t));
      } else {
        const int doubledX = 2 * (newBegin - seam) - (Width - 1);
        const int doubledY = 2 * y - (Height - 1);
        int32_t sourceX = Center + (cosine * doubledX + sine * doubledY) / 2;
        int32_t sourceY = Center + (-sine * doubledX + cosine * doubledY) / 2;
        for (int x = newBegin; x < end; ++x) {
          const int incomingX = x - seam;
          if (incomingX < begin || incomingX >= end) row[x] = 0;
          else if (exactQuarterTurn) row[x] = newImage[(sourceY / One) * Width + sourceX / One];
          else row[x] = interpolate(newImage, sourceX, sourceY);
          sourceX += cosine;
          sourceY -= sine;
        }
      }
    }
  }
}
void renderRipple(const uint16_t* oldImage, const uint16_t* newImage,
                  uint16_t* output, uint8_t progress,
                  bool rotateIncoming, uint16_t incomingAngle) {
  constexpr int32_t BandWidth = 12;  // Six pixels on each side of the wavefront.
  const int32_t radius = static_cast<int32_t>(progress) * Width / 255;
  const int32_t innerRadius = radius > BandWidth ? radius - BandWidth : 0;
  const int32_t outerRadius = radius + BandWidth;
  const int32_t radiusSquared = radius * radius;
  const int32_t innerSquared = innerRadius * innerRadius;
  const int32_t outerSquared = outerRadius * outerRadius;
  const bool drawBands = progress != 0 && progress != 255 && radius > BandWidth;
  const unsigned angle = incomingAngle % 36000U;
  int32_t cosine = One, sine = 0;
  if (rotateIncoming) rotationCoefficients(angle, cosine, sine);
  const bool exactQuarterTurn = angle % 9000U == 0U;

  for (int y = 0; y < Height; ++y) {
    const int begin = rowStart[y];
    const int end = rowEnd[y];
    uint16_t* row = output + static_cast<size_t>(y) * Width;
    clearOutside(row, begin, end);
    const int32_t dy = 2 * y - (Height - 1);
    const int32_t dySquared = dy * dy;
    const int32_t doubledX = 2 * begin - (Width - 1);
    int32_t sourceX = Center + (cosine * doubledX + sine * dy) / 2;
    int32_t sourceY = Center + (-sine * doubledX + cosine * dy) / 2;
    for (int x = begin; x < end; ++x) {
      const int32_t dx = 2 * x - (Width - 1);
      const int32_t distanceSquared = dx * dx + dySquared;
      const size_t index = static_cast<size_t>(x);
      const size_t sourceIndex = static_cast<size_t>(y) * Width + index;
      const bool revealed = progress == 255 ||
                            (progress != 0 && distanceSquared <= radiusSquared);
      uint16_t pixel;
      if (!revealed) {
        pixel = oldImage[sourceIndex];
      } else if (!rotateIncoming) {
        pixel = newImage[sourceIndex];
      } else if (exactQuarterTurn) {
        pixel = newImage[(sourceY / One) * Width + sourceX / One];
      } else {
        pixel = interpolate(newImage, sourceX, sourceY);
      }
      if (drawBands && distanceSquared >= innerSquared &&
          distanceSquared <= radiusSquared) {
        const uint32_t red = (pixel >> 11) & 31U;
        const uint32_t green = (pixel >> 5) & 63U;
        const uint32_t blue = pixel & 31U;
        pixel = static_cast<uint16_t>(((red + ((31U - red) >> 2)) << 11) |
                                      ((green + ((63U - green) >> 2)) << 5) |
                                      (blue + ((31U - blue) >> 2)));
      } else if (drawBands && distanceSquared > radiusSquared &&
                 distanceSquared <= outerSquared) {
        const uint32_t red = (pixel >> 11) & 31U;
        const uint32_t green = (pixel >> 5) & 63U;
        const uint32_t blue = pixel & 31U;
        pixel = static_cast<uint16_t>(((red - (red >> 2)) << 11) |
                                      ((green - (green >> 2)) << 5) |
                                      (blue - (blue >> 2)));
      }
      row[index] = pixel;
      sourceX += cosine;
      sourceY -= sine;
    }
  }
}

}  // namespace badge_animation
