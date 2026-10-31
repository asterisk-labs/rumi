// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pcodec's IntMult mode. The port is partial and geozl does
// not reproduce pcodec's wire format.

#include "decode_intmult_kernel.h"

#include <stdint.h>

// unsigned wrap on the multiply is defined, so a corrupt mult is a wrong value,
// never UB; validity folds into an accumulated flag
#define INTMULT_JOIN(T)                                                        \
  do {                                                                         \
    const T *m = (const T *)mults;                                             \
    const T *a = (const T *)adjs;                                              \
    T *d = (T *)dst;                                                           \
    const T b = (T)base;                                                       \
    unsigned bad = 0;                                                          \
    for (size_t k = 0; k < nb_elts; ++k) {                                     \
      const T adj = a[k];                                                      \
      bad |= (unsigned)(adj >= b);                                             \
      d[k] = (T)((T)(m[k] * b) + adj);                                         \
    }                                                                          \
    return (int)bad;                                                           \
  } while (0)

int intmult_join(void *dst, const void *mults, const void *adjs, size_t nb_elts,
                 size_t elt_width, uint64_t base) {
  if (base < 2)
    return 1;
  switch (elt_width) {
  case 1:
    INTMULT_JOIN(uint8_t);
  case 2:
    INTMULT_JOIN(uint16_t);
  case 4:
    INTMULT_JOIN(uint32_t);
  case 8:
    INTMULT_JOIN(uint64_t);
  default:
    return 1;
  }
}
