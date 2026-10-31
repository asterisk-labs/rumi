// Inverse horizontal predictor, one prefix sum scan per row. The scan is hand
// written because a prefix sum does not auto vectorize. Native width modular
// wraparound, no zigzag, matching the geozl residual convention.

#include "decode_delta_w_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#include "common/scan.h"

// The scans live in common/scan.h, shared by every horizontal predictor
// decoder. These aliases keep the local call sites unchanged.
#define scan8 geozl_scan8
#define scan16 geozl_scan16
#define scan32 geozl_scan32
#define scan64 geozl_scan64

#define DELTA_W_RUN(T, B)                                                      \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    for (size_t off = 0; off < nbElts; off += w)                               \
      scan##B(d + off, s + off, w);                                            \
  } while (0)

int delta_w_decode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, DELTA_W_RUN);
}

#undef DELTA_W_RUN

#undef scan8
#undef scan16
#undef scan32
#undef scan64
