// Inverse vertical predictor. The dependency runs between rows, perpendicular
// to the row we vectorize, so each row is a plain element wise add with the
// reconstructed row above.

#include "decode_delta_n_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>
#include <string.h>

#define DELTA_N_DEC(T, B)                                                      \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    memcpy(d, s, w * sizeof(T));                                               \
    for (size_t off = w; off < nbElts; off += w) {                             \
      const T *restrict up = d + off - w;                                      \
      const T *restrict res = s + off;                                         \
      T *restrict row = d + off;                                               \
      for (size_t c = 0; c < w; ++c)                                           \
        row[c] = (T)(res[c] + up[c]);                                          \
    }                                                                          \
  } while (0)

int delta_n_decode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, DELTA_N_DEC);
}

#undef DELTA_N_DEC
