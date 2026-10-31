// Forward MED predictor, the JPEG-LS median edge detector. The gradient
// W + N - NW is used only when it lies inside [min(W,N), max(W,N)], so its
// native width result never overflows.

#include "encode_med_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#define MED_FWD(T, B)                                                          \
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
        d[idx] = (T)(s[idx] - P);                                              \
      }                                                                        \
    }                                                                          \
  } while (0)

int med_encode(void *dst, const void *src, size_t width, size_t nbElts,
               size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, MED_FWD);
}

#undef MED_FWD
