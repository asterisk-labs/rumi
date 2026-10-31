#include "decode_planar_zigzag_transpose_pivco_kernel.h"

#include "common/raster.h"
#include "planar_zigzag/decode_planar_zigzag_kernel.h"

#include <stdint.h>

#define JOIN_LANES(T)                                                          \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      T z = 0;                                                                 \
      for (size_t k = 0; k < sizeof(T); ++k)                                   \
        z = (T)(z | ((T)lanes[k * nbElts + i] << (8 * k)));                    \
      d[i] = z;                                                                \
    }                                                                          \
  } while (0)

int planar_zigzag_transpose_pivco_unlanes(void *dst, const uint8_t *lanes,
                                          size_t width, size_t nbElts,
                                          size_t eltWidth, uint32_t planes) {
  if (dst == NULL || lanes == NULL || planes == 0 || nbElts == 0 ||
      nbElts % planes != 0)
    return 1;
  const size_t planeElts = nbElts / planes;
  if (geozl_row_width(width, planeElts) == 0)
    return 1;
  switch (eltWidth) {
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
  // The residuals are in place, so the planar decoder reads and writes dst.
  uint8_t *base = (uint8_t *)dst;
  for (uint32_t plane = 0; plane < planes; ++plane) {
    uint8_t *p = base + (size_t)plane * planeElts * eltWidth;
    if (planar_zigzag_decode(p, p, width, planeElts, eltWidth) != 0)
      return 1;
  }
  return 0;
}

#undef JOIN_LANES
