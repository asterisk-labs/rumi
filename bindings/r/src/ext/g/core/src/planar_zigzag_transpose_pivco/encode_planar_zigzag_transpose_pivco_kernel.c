#include "encode_planar_zigzag_transpose_pivco_kernel.h"

#include "common/raster.h"

#include <stdint.h>

#define PLANAR_ZIGZAG_LANES(T, B)                                              \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    const size_t rows = planeElts / width;                                     \
    for (size_t plane = 0; plane < planes; ++plane) {                          \
      const size_t planeBase = plane * planeElts;                              \
      for (size_t row = 0; row < rows; ++row) {                                \
        const size_t rowBase = planeBase + row * width;                        \
        for (size_t column = 0; column < width; ++column) {                    \
          const size_t pos = rowBase + column;                                 \
          const T Wv = (column > 0) ? s[pos - 1] : 0;                          \
          const T Nv = (row > 0) ? s[pos - width] : 0;                         \
          const T NWv = (row > 0 && column > 0) ? s[pos - width - 1] : 0;      \
          const T residual = (T)(s[pos] - (T)(Wv + Nv - NWv));                 \
          const T sign = (T)(0 - (residual >> ((B)-1)));                       \
          const T z = (T)((T)(residual << 1) ^ sign);                          \
          for (size_t k = 0; k < sizeof(T); ++k)                               \
            lanes[k * nbElts + pos] = (uint8_t)(z >> (8 * k));                 \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

int planar_zigzag_transpose_pivco_lanes(uint8_t *lanes, const void *src,
                                        size_t width, size_t nbElts,
                                        size_t eltWidth, uint32_t planes) {
  if (lanes == NULL || src == NULL || planes == 0 || nbElts == 0 ||
      nbElts % planes != 0)
    return 1;
  const size_t planeElts = nbElts / planes;
  if (geozl_row_width(width, planeElts) == 0)
    return 1;
  switch (eltWidth) {
  case 1:
    PLANAR_ZIGZAG_LANES(uint8_t, 8);
    break;
  case 2:
    PLANAR_ZIGZAG_LANES(uint16_t, 16);
    break;
  case 4:
    PLANAR_ZIGZAG_LANES(uint32_t, 32);
    break;
  case 8:
    PLANAR_ZIGZAG_LANES(uint64_t, 64);
    break;
  default:
    return 1;
  }
  return 0;
}

#undef PLANAR_ZIGZAG_LANES
