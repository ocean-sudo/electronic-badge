#pragma once
#include <cstdlib>
#include <cstddef>

constexpr unsigned MALLOC_CAP_SPIRAM = 1U;
constexpr unsigned MALLOC_CAP_8BIT = 2U;
constexpr unsigned MALLOC_CAP_DMA = 4U;
constexpr unsigned MALLOC_CAP_INTERNAL = 8U;

inline void *heap_caps_malloc(std::size_t size, unsigned) { return std::malloc(size); }
inline void *heap_caps_aligned_alloc(std::size_t alignment, std::size_t size, unsigned) {
  return std::aligned_alloc(alignment, size);
}
