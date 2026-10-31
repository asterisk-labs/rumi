// Forward planar predictor. Reads only from src and writes residuals to dst,
// so any traversal order is fine as long as dst does not alias src.

#include "encode_planar_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#define PLANAR_FWD(T, B)                                                       \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    size_t rows = nbElts / w;                                                  \
    for (size_t r = 0; r < rows; ++r) {                                        \
      for (size_t c = 0; c < w; ++c) {                                         \
        size_t idx = r * w + c;                                                \
        T Wv = (c > 0) ? s[idx - 1] : 0;                                       \
        T Nv = (r > 0) ? s[idx - w] : 0;                                       \
        T NWv = (r > 0 && c > 0) ? s[idx - w - 1] : 0;                         \
        d[idx] = (T)(s[idx] - (T)(Wv + Nv - NWv));                             \
      }                                                                        \
    }                                                                          \
  } while (0)

int planar_encode(void *dst, const void *src, size_t width, size_t nbElts,
                  size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, PLANAR_FWD);
}

#undef PLANAR_FWD
