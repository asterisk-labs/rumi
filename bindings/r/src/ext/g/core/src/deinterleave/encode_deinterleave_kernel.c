#include "encode_deinterleave_kernel.h"

#include <stdint.h>
#include <string.h>

// Pairwise so the compiler recognizes the interleave and emits vzip/unpck.
#define DEINTERLEAVE_SPLIT(T)                                                  \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    T *a = (T *)out0;                                                          \
    T *b = (T *)out1;                                                          \
    for (size_t k = 0; k < half; ++k) {                                        \
      a[k] = s[2 * k];                                                         \
      b[k] = s[2 * k + 1];                                                     \
    }                                                                          \
    if (nbElts & 1u)                                                           \
      a[half] = s[2 * half];                                                   \
  } while (0)

void deinterleave_split(void *out0, void *out1, const void *src, size_t nbElts,
                        size_t eltWidth) {
  const size_t half = nbElts / 2;
  switch (eltWidth) {
  case 1:
    DEINTERLEAVE_SPLIT(uint8_t);
    break;
  case 2:
    DEINTERLEAVE_SPLIT(uint16_t);
    break;
  case 4:
    DEINTERLEAVE_SPLIT(uint32_t);
    break;
  case 8:
    DEINTERLEAVE_SPLIT(uint64_t);
    break;
  default: {
    const char *s = (const char *)src;
    char *a = (char *)out0;
    char *b = (char *)out1;
    for (size_t k = 0; k < half; ++k) {
      memcpy(a + k * eltWidth, s + (2 * k) * eltWidth, eltWidth);
      memcpy(b + k * eltWidth, s + (2 * k + 1) * eltWidth, eltWidth);
    }
    if (nbElts & 1u)
      memcpy(a + half * eltWidth, s + (2 * half) * eltWidth, eltWidth);
  }
  }
}

#undef DEINTERLEAVE_SPLIT
