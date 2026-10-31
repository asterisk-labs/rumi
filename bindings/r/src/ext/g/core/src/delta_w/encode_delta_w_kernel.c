// Forward horizontal predictor. Offline path, plain scalar. Walk each row
// right to left so an in place transform keeps the source it still needs.

#include "encode_delta_w_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#define DELTA_W_FWD(T, B)                                                      \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    for (size_t off = 0; off < nbElts; off += w) {                             \
      for (size_t c = w; c-- > 1;)                                             \
        d[off + c] = (T)(s[off + c] - s[off + c - 1]);                         \
      d[off] = s[off];                                                         \
    }                                                                          \
  } while (0)

int delta_w_encode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, DELTA_W_FWD);
}

#undef DELTA_W_FWD
