// Forward planar predictor and Zigzag in one pass.

#include "encode_planar_zigzag_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#define PLANAR_ZIGZAG_FWD(T, B)                                               \
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
        T residual = (T)(s[idx] - (T)(Wv + Nv - NWv));                         \
        T sign = (T)(0 - (residual >> (B - 1)));                               \
        d[idx] = (T)((T)(residual << 1) ^ sign);                               \
      }                                                                        \
    }                                                                          \
  } while (0)

int planar_zigzag_encode(void *dst, const void *src, size_t width,
                         size_t nbElts, size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, PLANAR_ZIGZAG_FWD);
}

#undef PLANAR_ZIGZAG_FWD
