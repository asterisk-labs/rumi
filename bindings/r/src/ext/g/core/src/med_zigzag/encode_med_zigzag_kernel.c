// Forward MED predictor and Zigzag in one pass. The median keeps its three way
// select here: encode predicts from the source, so there is no chain to break
// and the loop vectorizes, where a masked select would only add work.

#include "encode_med_zigzag_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#define MED_ZIGZAG_FWD(T, B)                                                   \
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
        T mn = Wv < Nv ? Wv : Nv;                                              \
        T mx = Wv < Nv ? Nv : Wv;                                              \
        T P = (NWv >= mx) ? mn : (NWv <= mn) ? mx : (T)(Wv + Nv - NWv);        \
        T residual = (T)(s[idx] - P);                                          \
        T sign = (T)(0 - (residual >> (B - 1)));                               \
        d[idx] = (T)((T)(residual << 1) ^ sign);                               \
      }                                                                        \
    }                                                                          \
  } while (0)

int med_zigzag_encode(void *dst, const void *src, size_t width, size_t nbElts,
                      size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, MED_ZIGZAG_FWD);
}

#undef MED_ZIGZAG_FWD
