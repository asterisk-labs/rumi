#include "decode_transpose_pivco_kernel.h"

#include <string.h>

#define JOIN_LANES(T)                                                          \
  do {                                                                         \
    uint8_t *bytes = (uint8_t *)dst;                                           \
    for (size_t i = 0; i < count; ++i) {                                       \
      T value = 0;                                                             \
      for (size_t lane = 0; lane < sizeof(T); ++lane)                          \
        value |= (T)((T)lanes[lane * count + i] << (8 * lane));                \
      memcpy(bytes + i * sizeof(T), &value, sizeof(T));                        \
    }                                                                          \
  } while (0)

int transpose_pivco_unlanes(void *dst, const uint8_t *lanes, size_t count,
                            size_t width) {
  if (dst == NULL || lanes == NULL || count == 0 ||
      (width != 1 && width != 2 && width != 4 && width != 8))
    return 1;
  switch (width) {
  case 1:
    JOIN_LANES(uint8_t);
    break;
  case 2:
    JOIN_LANES(uint16_t);
    break;
  case 4:
    JOIN_LANES(uint32_t);
    break;
  case 8:
    JOIN_LANES(uint64_t);
    break;
  default:
    return 1;
  }
  return 0;
}

#undef JOIN_LANES
