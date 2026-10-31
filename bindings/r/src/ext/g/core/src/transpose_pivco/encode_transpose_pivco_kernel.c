#include "encode_transpose_pivco_kernel.h"

#include <string.h>

#define SPLIT_LANES(T)                                                         \
  do {                                                                         \
    const uint8_t *bytes = (const uint8_t *)src;                               \
    for (size_t i = 0; i < count; ++i) {                                       \
      T value;                                                                 \
      memcpy(&value, bytes + i * sizeof(T), sizeof(T));                        \
      for (size_t lane = 0; lane < sizeof(T); ++lane)                          \
        lanes[lane * count + i] = (uint8_t)(value >> (8 * lane));              \
    }                                                                          \
  } while (0)

int transpose_pivco_lanes(uint8_t *lanes, const void *src, size_t count,
                          size_t width) {
  if (lanes == NULL || src == NULL || count == 0 ||
      (width != 1 && width != 2 && width != 4 && width != 8))
    return 1;
  switch (width) {
  case 1:
    SPLIT_LANES(uint8_t);
    break;
  case 2:
    SPLIT_LANES(uint16_t);
    break;
  case 4:
    SPLIT_LANES(uint32_t);
    break;
  case 8:
    SPLIT_LANES(uint64_t);
    break;
  default:
    return 1;
  }
  return 0;
}

#undef SPLIT_LANES
