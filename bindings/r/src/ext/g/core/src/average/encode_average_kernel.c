// Forward Average predictor (PNG filter 3). The floor average of W and N is
// computed as (W>>1) + (N>>1) + (W&N&1) so the sum never overflows the sample
// width.

#include "encode_average_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#define AVERAGE_FWD(T, B)                                                      \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    size_t rows = nbElts / w;                                                  \
    for (size_t r = 0; r < rows; ++r) {                                        \
      for (size_t c = 0; c < w; ++c) {                                         \
        size_t idx = r * w + c;                                                \
        T Wv = (c > 0) ? s[idx - 1] : 0;                                       \
        T Nv = (r > 0) ? s[idx - w] : 0;                                       \
        T P = (T)((Wv >> 1) + (Nv >> 1) + (Wv & Nv & 1));                      \
        d[idx] = (T)(s[idx] - P);                                              \
      }                                                                        \
    }                                                                          \
  } while (0)

int average_encode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, AVERAGE_FWD);
}

#undef AVERAGE_FWD
