// Inverse Zigzag and MED reconstruction in one pass. The traversal is the one
// med uses, from decode_med_wavefront.h, with the Zigzag undone where each
// sample is read, so no intermediate residual stream is written or reread.

#include "decode_med_zigzag_kernel.h"

#include "common/raster.h" // geozl_row_width
#include "med/decode_med_wavefront.h"

#include <stdint.h>

#define MED_ZIGZAG_RES(T, V) ((T)(((T)(V) >> 1) ^ (T)(0 - ((T)(V) & 1))))

#define MED_ZIGZAG_DEC(T, B) GEOZL_MED_DECODE(T, MED_ZIGZAG_RES)

int med_zigzag_decode(void *dst, const void *src, size_t width, size_t nbElts,
                      size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, MED_ZIGZAG_DEC);
}

#undef MED_ZIGZAG_DEC
#undef MED_ZIGZAG_RES
