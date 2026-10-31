// Inverse Average predictor, PNG filter 3 (RFC 2083 6.5,
// https://datatracker.ietf.org/doc/rfc2083/).
//
// floor((W + N) / 2) chains through W, so it is an IIR, not a prefix sum, and
// cannot vectorize like delta_n or planar. But the N side is known before the
// row starts, so it is hoisted into a vectorizable prepass (B[c], L[c]) leaving
// a minimal W chain. The split rests on the identity
//   floor((W+N)/2) == (W>>1) + (N>>1) + (W & N & 1),
// with L[c] = N & 1 carrying the low bit, so the chain stays in native width
// with no overflow. Column zero has W == 0, so t starts at floor(N/2), the edge
// rule. If the scratch allocation fails the kernel falls back to the in-place
// recurrence, so it never fails on a valid frame. @dst may alias @src.

#include "decode_average_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>
#include <stdlib.h>

#define AVERAGE_DEC_DIRECT(T)                                                  \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    const size_t rows = nbElts / w;                                            \
    d[0] = s[0];                                                               \
    for (size_t c = 1; c < w; ++c)                                             \
      d[c] = (T)(s[c] + (d[c - 1] >> 1));                                      \
    for (size_t r = 1; r < rows; ++r) {                                        \
      const size_t row = r * w;                                                \
      d[row] = (T)(s[row] + (d[row - w] >> 1));                                \
      for (size_t c = 1; c < w; ++c) {                                         \
        const size_t i = row + c;                                              \
        const T Wv = d[i - 1];                                                 \
        const T Nv = d[i - w];                                                 \
        const T P = (T)((Wv >> 1) + (Nv >> 1) + (Wv & Nv & 1));                \
        d[i] = (T)(s[i] + P);                                                  \
      }                                                                        \
    }                                                                          \
  } while (0)

#define AVERAGE_DEC_SPLIT(T)                                                   \
  do {                                                                         \
    T *restrict B = (T *)scratch;                                              \
    T *restrict L = (T *)scratch + w;                                          \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    const size_t rows = nbElts / w;                                            \
    d[0] = s[0];                                                               \
    for (size_t c = 1; c < w; ++c)                                             \
      d[c] = (T)(s[c] + (d[c - 1] >> 1));                                      \
    for (size_t r = 1; r < rows; ++r) {                                        \
      const size_t row = r * w;                                                \
      const T *restrict up = d + row - w;                                      \
      const T *restrict res = s + row;                                         \
      T *restrict o = d + row;                                                 \
      for (size_t c = 0; c < w; ++c) {                                         \
        B[c] = (T)(res[c] + (up[c] >> 1));                                     \
        L[c] = (T)(up[c] & 1);                                                 \
      }                                                                        \
      T t = B[0];                                                              \
      o[0] = t;                                                                \
      for (size_t c = 1; c < w; ++c) {                                         \
        t = (T)(B[c] + (t >> 1) + (t & L[c]));                                 \
        o[c] = t;                                                              \
      }                                                                        \
    }                                                                          \
  } while (0)

int average_decode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth) {
  // A width that does not divide nbElts would leave the last row short,
  // and the row loops below assume every row is complete.
  const size_t w = geozl_row_width(width, nbElts);
  if (w == 0)
    return 1;

  // Two scratch rows (B and L); on allocation failure fall back in place.
  void *scratch = malloc((size_t)2 * w * eltWidth);
  if (scratch == NULL) {
    switch (eltWidth) {
    case 1:
      AVERAGE_DEC_DIRECT(uint8_t);
      break;
    case 2:
      AVERAGE_DEC_DIRECT(uint16_t);
      break;
    case 4:
      AVERAGE_DEC_DIRECT(uint32_t);
      break;
    case 8:
      AVERAGE_DEC_DIRECT(uint64_t);
      break;
    default:
      return 1; // eltWidth must be 1, 2, 4 or 8
    }
    return 0;
  }

  switch (eltWidth) {
  case 1:
    AVERAGE_DEC_SPLIT(uint8_t);
    break;
  case 2:
    AVERAGE_DEC_SPLIT(uint16_t);
    break;
  case 4:
    AVERAGE_DEC_SPLIT(uint32_t);
    break;
  case 8:
    AVERAGE_DEC_SPLIT(uint64_t);
    break;
  default:
    free(scratch);
    return 1; // eltWidth must be 1, 2, 4 or 8
  }

  free(scratch);
  return 0;
}

#undef AVERAGE_DEC_DIRECT
#undef AVERAGE_DEC_SPLIT