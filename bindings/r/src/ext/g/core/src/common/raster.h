#ifndef GEOZL_COMMON_RASTER_H
#define GEOZL_COMMON_RASTER_H

#include <stddef.h>
#include <stdint.h>

// Return width when it tiles nbElts, otherwise 0.
static inline size_t geozl_row_width(size_t width, size_t nbElts) {
  if (nbElts == 0)
    return 0;
  if (width == 0 || width > nbElts)
    return 0;
  return (nbElts % width == 0) ? width : 0;
}

// Normalize 0 or an oversized width to a single row.
static inline uint32_t geozl_row_width_declared(uint32_t width, size_t nbElts) {
  return (width == 0 || (size_t)width > nbElts) ? (uint32_t)nbElts : width;
}

// Row width plus the element width switch, written out identically by nine
// kernels. B is the width in bits, for bodies that paste it into a scan name.
// dst and src must be aligned to eltWidth; checking it here costs delta_w u64
// a third, so it belongs where an unaligned pointer first appears.
#define GEOZL_ROW_DISPATCH(W, BODY)                                            \
  do {                                                                         \
    const size_t W = geozl_row_width(width, nbElts);                           \
    if (W == 0)                                                                \
      return 1;                                                                \
    switch (eltWidth) {                                                        \
    case 1:                                                                    \
      BODY(uint8_t, 8);                                                        \
      break;                                                                   \
    case 2:                                                                    \
      BODY(uint16_t, 16);                                                      \
      break;                                                                   \
    case 4:                                                                    \
      BODY(uint32_t, 32);                                                      \
      break;                                                                   \
    case 8:                                                                    \
      BODY(uint64_t, 64);                                                      \
      break;                                                                   \
    default:                                                                   \
      return 1; /* an OpenZL numeric stream is 1, 2, 4 or 8 */                 \
    }                                                                          \
    return 0;                                                                  \
  } while (0)


// Return planes when it splits the stream into whole rows, otherwise 1.
static inline uint32_t geozl_planes_declared(uint32_t planes, uint32_t width,
                                             size_t nbElts) {
  if (planes <= 1 || width == 0 || nbElts == 0)
    return 1;
  if (nbElts % planes != 0)
    return 1;
  const size_t per = nbElts / planes;
  if (per == 0 || per % width != 0)
    return 1;
  return planes;
}

#endif // GEOZL_COMMON_RASTER_H
