#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_LAYOUT_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_LAYOUT_H

#include "pfor/pfor_check.h"

#include <stddef.h>
#include <stdint.h>

#define GEOZL_PZP_BAND 16u
// Prediction restarts at strip boundaries. Keep this a multiple of 256.
#define GEOZL_PZP_STRIP 1024u
// Bit width, exception count and format.
#define GEOZL_PZP_HEADER_BYTES 3u

// Full bands have 16 rows; the tail uses descending powers of two.
static inline unsigned geozl_pzp_band_height(size_t rowsLeft) {
  if (rowsLeft >= GEOZL_PZP_BAND)
    return GEOZL_PZP_BAND;
  unsigned h = 8;
  while (h > rowsLeft)
    h >>= 1;
  return h;
}

static inline size_t geozl_pzp_block_width(unsigned h) {
  return GEOZL_PFOR_BLOCK / h;
}

static inline size_t geozl_pzp_slot(unsigned h, size_t i, size_t j) {
  return 8u * (i + (size_t)h * (j >> 3)) + (j & 7u);
}

// Every block contains at least one sample, so this cannot exceed width * rows.
static inline size_t geozl_pzp_plane_blocks(size_t width, size_t rows) {
  size_t blocks = (rows / GEOZL_PZP_BAND) *
                  (width / GEOZL_PZP_BAND + (width % GEOZL_PZP_BAND != 0));
  for (unsigned h = GEOZL_PZP_BAND / 2; h != 0; h >>= 1) {
    if ((rows & h) == 0)
      continue;
    const size_t bw = geozl_pzp_block_width(h);
    blocks += width / bw + (width % bw != 0);
  }
  return blocks;
}

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_LAYOUT_H
