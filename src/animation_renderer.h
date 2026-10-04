#pragma once

#include <stdint.h>

namespace badge_animation {

constexpr int Width = 466;
constexpr int Height = 466;

// Call before rendering. Repeated calls are harmless; the row table is built once.
void initialize();

// Every image is Width * Height RGB565 pixels. Output must not alias either source.
// Pixels outside the radius-233 circle centered on (232.5, 232.5) are black.
// Positive angles rotate clockwise; angles are normalized modulo 36000.
void renderRotation(const uint16_t* source, uint16_t* output,
                    uint16_t angleCentiDegrees);

// Positive offsets move the source right/down; uncovered pixels are black.
void renderShift(const uint16_t* source, uint16_t* output, int dx, int dy);

// Offset is clamped to Width: old moves left, new enters from the right.
// rotateIncoming samples the canonical new image at incomingAngle (clockwise)
// without scratch buffers; the outgoing image is already in its presented pose.
void renderSlide(const uint16_t* oldImage, const uint16_t* newImage,
                 uint16_t* output, unsigned offset,
                 bool rotateIncoming = false, uint16_t incomingAngle = 0);

}  // namespace badge_animation
