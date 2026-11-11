#include "encode_planar_zigzag_pfor_kernel.h"
#include "planar_zigzag_pfor_layout.h"

#include "common/raster.h"
#include "pfor/encode_pfor_kernel.h"
#include "pfor/pfor_check.h"

#include <stdint.h>
#include <string.h>

static size_t pzp_blocks(size_t nbElts, size_t width, uint32_t planes) {
  if (nbElts == 0 || planes == 0 || nbElts % planes != 0)
    return 0;
  const size_t planeElts = nbElts / planes;
  if (geozl_row_width(width, planeElts) == 0)
    return 0;
  const size_t perPlane = geozl_pzp_plane_blocks(width, planeElts / width);
  if (perPlane > SIZE_MAX / planes)
    return 0;
  return perPlane * planes;
}

size_t planar_zigzag_pfor_bound(size_t nbElts, size_t eltWidth, size_t width,
                                uint32_t planes) {
  if (!geozl_pfor_width_ok(eltWidth))
    return 0;
  const size_t blocks = pzp_blocks(nbElts, width, planes);
  const size_t perBlock =
      GEOZL_PZP_HEADER_BYTES + (size_t)GEOZL_PFOR_BLOCK * eltWidth;
  if (blocks == 0 || blocks > SIZE_MAX / perBlock)
    return 0;
  return blocks * perBlock;
}

// Keep headers contiguous and append block bodies after them.
#define PLANAR_ZIGZAG_PFOR_ENC(T, B)                                           \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    T *z = (T *)scratch;                                                       \
    for (size_t plane = 0; plane < planes; ++plane) {                          \
      const T *ps = s + plane * planeElts;                                     \
      for (size_t y0 = 0; y0 < rows;) {                                        \
        const unsigned h = geozl_pzp_band_height(rows - y0);                   \
        const size_t bw = geozl_pzp_block_width(h);                            \
        for (size_t x0 = 0; x0 < width; x0 += bw) {                            \
          const size_t vw = (width - x0 < bw) ? width - x0 : bw;               \
          if (vw != bw)                                                        \
            memset(z, 0, GEOZL_PFOR_BLOCK * sizeof(T));                        \
          for (unsigned i = 0; i < h; ++i) {                                   \
            const T *row = ps + (y0 + i) * width;                              \
            const T *up = (i > 0) ? row - width : row;                         \
            for (size_t j = 0; j < vw; ++j) {                                  \
              const size_t x = x0 + j;                                         \
              const int left = x % GEOZL_PZP_STRIP != 0;                       \
              const T Wv = left ? row[x - 1] : 0;                              \
              const T Nv = (i > 0) ? up[x] : 0;                                \
              const T NWv = (i > 0 && left) ? up[x - 1] : 0;                   \
              const T residual = (T)(row[x] - (T)(Wv + Nv - NWv));             \
              const T sign = (T)(0 - (residual >> ((B) - 1)));                 \
              z[geozl_pzp_slot(h, i, j)] = (T)((T)(residual << 1) ^ sign);     \
            }                                                                  \
          }                                                                    \
          const size_t used =                                                  \
              pfor_encode_block_split(hdr, body, z, sizeof(T));                \
          if (used == SIZE_MAX)                                                \
            return 1;                                                          \
          hdr += GEOZL_PZP_HEADER_BYTES;                                       \
          body += used;                                                        \
        }                                                                      \
        y0 += h;                                                               \
      }                                                                        \
    }                                                                          \
  } while (0)

int planar_zigzag_pfor_encode(void *dst, size_t dstCapacity, size_t *outSize,
                              const void *src, size_t width, size_t nbElts,
                              size_t eltWidth, uint32_t planes) {
  if (dst == NULL || src == NULL || outSize == NULL)
    return 1;
  const size_t bound =
      planar_zigzag_pfor_bound(nbElts, eltWidth, width, planes);
  if (bound == 0 || dstCapacity < bound)
    return 1;
  const size_t planeElts = nbElts / planes;
  const size_t rows = planeElts / width;
  const size_t blocks = pzp_blocks(nbElts, width, planes);

  uint8_t *hdr = (uint8_t *)dst;
  uint8_t *body = hdr + blocks * GEOZL_PZP_HEADER_BYTES;
  uint64_t scratch[GEOZL_PFOR_BLOCK];
  switch (eltWidth) {
  case 1:
    PLANAR_ZIGZAG_PFOR_ENC(uint8_t, 8);
    break;
  case 2:
    PLANAR_ZIGZAG_PFOR_ENC(uint16_t, 16);
    break;
  case 4:
    PLANAR_ZIGZAG_PFOR_ENC(uint32_t, 32);
    break;
  case 8:
    PLANAR_ZIGZAG_PFOR_ENC(uint64_t, 64);
    break;
  default:
    return 1;
  }
  *outSize = (size_t)(body - (uint8_t *)dst);
  return 0;
}

#undef PLANAR_ZIGZAG_PFOR_ENC
