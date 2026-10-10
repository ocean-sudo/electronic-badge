#pragma once
#include <cstdint>

extern uint32_t pictureAnimationTestMillis;
extern uint32_t pictureAnimationTestMicros;
extern void (*pictureAnimationTestDelayHook)();

inline uint32_t millis() { return pictureAnimationTestMillis; }
inline uint32_t micros() { return pictureAnimationTestMicros; }
inline void delay(uint32_t ms) {
  pictureAnimationTestMillis += ms;
  pictureAnimationTestMicros += ms * 1000U;
  if (pictureAnimationTestDelayHook) pictureAnimationTestDelayHook();
}
inline void delayMicroseconds(uint32_t us) { pictureAnimationTestMicros += us; }
