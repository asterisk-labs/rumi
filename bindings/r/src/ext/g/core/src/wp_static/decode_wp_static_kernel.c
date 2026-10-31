// Inverse wp_static predictor, W + K + residual with K = (cN*N + cNW*NW +
// cNE*NE + cNN*NN + round) >> shift over the row above. K has no left
// dependency, so each row folds K into the residual and a prefix sum resolves
// the W chain, the same scan as planar. K sums unsigned then shifts signed, so
// a u64 tile wraps at 2^64 rather than overflowing.
//
// For 1 and 2 byte samples the fold accumulates in 32 bits when shift <= 16.
// The output is <= 16 bits and only sum bits [shift, shift+16) reach it, so
// with shift <= 16 they stay below bit 32 and survive the wrap, matching the
// 64-bit fold exactly while letting the compiler vectorize the multiplies. A
// wider sample or a larger shift (no encoder emits one, a corrupt header might)
// takes the 64-bit path. @dst may alias @src.

#include "decode_wp_static_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stddef.h>
#include <stdint.h>

#include "common/scan.h"

#define scan8 geozl_scan8
#define scan16 geozl_scan16
#define scan32 geozl_scan32
#define scan64 geozl_scan64

// Fold d[c] = res[c] + K[c]. Edge taps (W, NW at column 0, NE at the last
// column, NN on row 1) are zero, so the two end columns are peeled. FAST picks
// the 32-bit interior for 1 and 2 byte samples; it compiles out otherwise.

#define WP_STATIC_KROW(T, NAME, FAST)                                          \
  static void NAME(T *d, const T *res, const T *ab, const T *ab2, size_t n,    \
                   uint64_t cN, uint64_t cNW, uint64_t cNE, uint64_t cNN,      \
                   uint64_t rnd, int sh) {                                     \
    {                                                                          \
      uint64_t acc = cN * ab[0] + (n > 1 ? cNE * ab[1] : 0) +                  \
                     (ab2 ? cNN * ab2[0] : 0) + rnd;                           \
      d[0] = (T)(res[0] + (T)((int64_t)acc >> sh));                            \
    }                                                                          \
    size_t c = 1;                                                              \
    if ((FAST) && sh <= 16) {                                                  \
      const uint32_t kN = (uint32_t)cN, kNW = (uint32_t)cNW,                   \
                     kNE = (uint32_t)cNE, kNN = (uint32_t)cNN,                 \
                     krd = (uint32_t)rnd;                                      \
      if (ab2) {                                                               \
        for (; c + 1 < n; ++c) {                                               \
          uint32_t acc = kN * ab[c] + kNW * ab[c - 1] + kNE * ab[c + 1] +      \
                         kNN * ab2[c] + krd;                                   \
          d[c] = (T)(res[c] + (T)((int32_t)acc >> sh));                        \
        }                                                                      \
      } else {                                                                 \
        for (; c + 1 < n; ++c) {                                               \
          uint32_t acc = kN * ab[c] + kNW * ab[c - 1] + kNE * ab[c + 1] + krd; \
          d[c] = (T)(res[c] + (T)((int32_t)acc >> sh));                        \
        }                                                                      \
      }                                                                        \
    } else if (ab2) {                                                          \
      for (; c + 1 < n; ++c) {                                                 \
        uint64_t acc = cN * ab[c] + cNW * ab[c - 1] + cNE * ab[c + 1] +        \
                       cNN * ab2[c] + rnd;                                     \
        d[c] = (T)(res[c] + (T)((int64_t)acc >> sh));                          \
      }                                                                        \
    } else {                                                                   \
      for (; c + 1 < n; ++c) {                                                 \
        uint64_t acc = cN * ab[c] + cNW * ab[c - 1] + cNE * ab[c + 1] + rnd;   \
        d[c] = (T)(res[c] + (T)((int64_t)acc >> sh));                          \
      }                                                                        \
    }                                                                          \
    if (n > 1) {                                                               \
      const size_t L = n - 1;                                                  \
      uint64_t acc =                                                           \
          cN * ab[L] + cNW * ab[L - 1] + (ab2 ? cNN * ab2[L] : 0) + rnd;       \
      d[L] = (T)(res[L] + (T)((int64_t)acc >> sh));                            \
    }                                                                          \
  }

WP_STATIC_KROW(uint8_t, krow8, 1)
WP_STATIC_KROW(uint16_t, krow16, 1)
WP_STATIC_KROW(uint32_t, krow32, 0)
WP_STATIC_KROW(uint64_t, krow64, 0)

#undef WP_STATIC_KROW

// Row 0 is a bare prefix sum; later rows fold K first. ab2 is null until row 2.

#define WP_STATIC_DEC(T, SCAN, KROW)                                           \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    const uint64_t cN = (uint64_t)coeffs[0], cNW = (uint64_t)coeffs[1],        \
                   cNE = (uint64_t)coeffs[2], cNN = (uint64_t)coeffs[3];       \
    const uint64_t rnd = shift ? (uint64_t)1 << (shift - 1) : 0;               \
    SCAN(d, s, w);                                                             \
    for (size_t off = w, r = 1; off < nbElts; off += w, ++r) {                 \
      const T *ab2 = (r >= 2) ? d + off - 2 * w : (const T *)0;                \
      KROW(d + off, s + off, d + off - w, ab2, w, cN, cNW, cNE, cNN, rnd,      \
           shift);                                                             \
      SCAN(d + off, d + off, w);                                               \
    }                                                                          \
  } while (0)

int wp_static_decode(void *dst, const void *src, size_t width, size_t nbElts,
                     size_t eltWidth, const int16_t coeffs[4], uint8_t shift) {
  // The sum below folds in 64 bits, so a wider shift is undefined. Checked here
  // and not only in the binding, since the kernel is exported and a caller that
  // never went through a frame can reach it.
  if (shift >= 64)
    return 1;
  // A width that does not divide nbElts would leave the last row short,
  // and the row loops below assume every row is complete.
  const size_t w = geozl_row_width(width, nbElts);
  if (w == 0)
    return 1;
  switch (eltWidth) {
  case 1:
    WP_STATIC_DEC(uint8_t, scan8, krow8);
    break;
  case 2:
    WP_STATIC_DEC(uint16_t, scan16, krow16);
    break;
  case 4:
    WP_STATIC_DEC(uint32_t, scan32, krow32);
    break;
  case 8:
    WP_STATIC_DEC(uint64_t, scan64, krow64);
    break;
  default:
    return 1; // eltWidth must be 1, 2, 4 or 8
  }
  return 0;
}

#undef WP_STATIC_DEC

#undef scan8
#undef scan16
#undef scan32
#undef scan64
