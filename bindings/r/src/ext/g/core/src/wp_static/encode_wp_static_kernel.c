// Forward wp_static predictor. Reads only from src, so any traversal order is
// fine as long as dst does not alias src. The sum folds unsigned in 64-bit so a
// u64 tile cannot overflow it, then the shift reads it back signed. The decoder
// folds the same sum.

#include "encode_wp_static_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stddef.h>
#include <stdint.h>

#define WP_STATIC_FWD(T)                                                       \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    const size_t rows = nbElts / w;                                            \
    const uint64_t cN = (uint64_t)coeffs[0], cNW = (uint64_t)coeffs[1],        \
                   cNE = (uint64_t)coeffs[2], cNN = (uint64_t)coeffs[3];       \
    const uint64_t rnd = shift ? (uint64_t)1 << (shift - 1) : 0;               \
    for (size_t r = 0; r < rows; ++r) {                                        \
      const size_t row = r * w;                                                \
      const T *ab = (r >= 1) ? s + row - w : (const T *)0;                     \
      const T *ab2 = (r >= 2) ? s + row - 2 * w : (const T *)0;                \
      for (size_t c = 0; c < w; ++c) {                                         \
        const uint64_t N = ab ? ab[c] : 0;                                     \
        const uint64_t NW = (ab && c > 0) ? ab[c - 1] : 0;                     \
        const uint64_t NE = (ab && c + 1 < w) ? ab[c + 1] : 0;                 \
        const uint64_t NN = ab2 ? ab2[c] : 0;                                  \
        const uint64_t acc = cN * N + cNW * NW + cNE * NE + cNN * NN + rnd;    \
        const int64_t K = (int64_t)acc >> shift;                               \
        const uint64_t W = (c > 0) ? s[row + c - 1] : 0;                       \
        d[row + c] = (T)(s[row + c] - (W + (uint64_t)K));                      \
      }                                                                        \
    }                                                                          \
  } while (0)

int wp_static_encode(void *dst, const void *src, size_t width, size_t nbElts,
                     size_t eltWidth, const int16_t coeffs[4], uint8_t shift) {
  // A width that does not divide nbElts would leave the last row short,
  // and the row loops below assume every row is complete.
  const size_t w = geozl_row_width(width, nbElts);
  if (w == 0)
    return 1;
  switch (eltWidth) {
  case 1:
    WP_STATIC_FWD(uint8_t);
    break;
  case 2:
    WP_STATIC_FWD(uint16_t);
    break;
  case 4:
    WP_STATIC_FWD(uint32_t);
    break;
  case 8:
    WP_STATIC_FWD(uint64_t);
    break;
  default:
    return 1; // eltWidth must be 1, 2, 4 or 8
  }
  return 0;
}

#undef WP_STATIC_FWD
