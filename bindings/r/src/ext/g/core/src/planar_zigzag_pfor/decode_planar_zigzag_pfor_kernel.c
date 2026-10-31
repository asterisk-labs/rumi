#include "decode_planar_zigzag_pfor_kernel.h"

#include "common/raster.h"
#include "pfor/decode_pfor_kernel.h"
#include "pfor/pfor_check.h"
#include "planar_zigzag/decode_planar_zigzag_kernel.h"

#include <stdint.h>

int planar_zigzag_pfor_decode(void *dst, size_t width, size_t nbElts,
                              size_t eltWidth, uint32_t planes,
                              const void *src, size_t srcSize) {
  if (dst == NULL || src == NULL || planes == 0 || nbElts == 0 ||
      nbElts % planes != 0)
    return 1;
  const size_t planeElts = nbElts / planes;
  if (geozl_row_width(width, planeElts) == 0)
    return 1;

  const uint8_t *p = (const uint8_t *)src;
  size_t left = srcSize;
  uint64_t scratch[GEOZL_PFOR_BLOCK];
  for (size_t off = 0; off < nbElts; off += GEOZL_PFOR_BLOCK) {
    const size_t have = nbElts - off;
    const size_t n =
        (have < GEOZL_PFOR_BLOCK) ? have : GEOZL_PFOR_BLOCK;
    const size_t used = pfor_decode_block(scratch, eltWidth, p, left);
    if (used == 0)
      return 1;
    if (planar_zigzag_decode_stream(dst, scratch, off, n, width, planeElts,
                                    eltWidth) != 0)
      return 1;
    p += used;
    left -= used;
  }
  return left == 0 ? 0 : 1;
}
