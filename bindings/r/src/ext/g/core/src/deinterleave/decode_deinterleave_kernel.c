#include "decode_deinterleave_kernel.h"

#include <stdint.h>
#include <string.h>

// Pairwise so the compiler recognizes the interleave and emits vzip/unpck.
#define DEINTERLEAVE_JOIN(T)                                                   \
  do {                                                                         \
    const T *a = (const T *)in0;                                               \
    const T *b = (const T *)in1;                                               \
    T *d = (T *)dst;                                                           \
    for (size_t k = 0; k < half; ++k) {                                        \
      d[2 * k] = a[k];                                                         \
      d[2 * k + 1] = b[k];                                                     \
    }                                                                          \
    if (nbElts & 1u)                                                           \
      d[2 * half] = a[half];                                                   \
  } while (0)

void deinterleave_join(void *dst, const void *in0, const void *in1,
                       size_t nbElts, size_t eltWidth) {
  const size_t half = nbElts / 2;
  switch (eltWidth) {
  case 1:
    DEINTERLEAVE_JOIN(uint8_t);
    break;
  case 2:
    DEINTERLEAVE_JOIN(uint16_t);
    break;
  case 4:
    DEINTERLEAVE_JOIN(uint32_t);
    break;
  case 8:
    DEINTERLEAVE_JOIN(uint64_t);
    break;
  default: {
    const char *a = (const char *)in0;
    const char *b = (const char *)in1;
    char *d = (char *)dst;
    for (size_t k = 0; k < half; ++k) {
      memcpy(d + (2 * k) * eltWidth, a + k * eltWidth, eltWidth);
      memcpy(d + (2 * k + 1) * eltWidth, b + k * eltWidth, eltWidth);
    }
    if (nbElts & 1u)
      memcpy(d + (2 * half) * eltWidth, a + half * eltWidth, eltWidth);
  }
  }
}

#undef DEINTERLEAVE_JOIN
