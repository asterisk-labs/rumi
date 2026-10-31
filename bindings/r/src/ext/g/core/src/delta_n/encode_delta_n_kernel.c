// Forward vertical predictor. Walk rows bottom to top so an in place transform
// still has the original row above when it needs it.

#include "encode_delta_n_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#define DELTA_N_FWD(T, B)                                                      \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    for (size_t off = nbElts; off > w;) {                                      \
      off -= w;                                                                \
      const T *up = s + off - w;                                               \
      const T *row = s + off;                                                  \
      T *o = d + off;                                                          \
      for (size_t c = 0; c < w; ++c)                                           \
        o[c] = (T)(row[c] - up[c]);                                            \
    }                                                                          \
    for (size_t c = 0; c < w; ++c)                                             \
      d[c] = s[c];                                                             \
  } while (0)

int delta_n_encode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, DELTA_N_FWD);
}

#undef DELTA_N_FWD
