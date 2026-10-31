// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pcodec's FloatQuant mode and the ordered latent mapping
// in pco/src/data_types/float.rs. The port is partial and geozl does not
// reproduce pcodec's wire format.

#include "decode_floatquant_kernel.h"

#include <stdint.h>

// from_latent_ordered inverts to_latent_ordered: a key with the sign bit set
// came from a non negative float, so clear the sign bit, otherwise flip all
// bits. The sign of the original float is recovered from the high part, since
// non negative floats have keys at or above the midpoint (the sign bit), so
// primary is at or above sign_cutoff = sign_bit >> k.
#define FLOATQUANT_JOIN(T, SIGN)                                               \
  do {                                                                         \
    const T *p = (const T *)primary;                                           \
    const T *q = (const T *)secondary;                                         \
    T *d = (T *)dst;                                                           \
    const T maxlow = (T)(((T)1 << k) - (T)1);                                  \
    const T sign_cutoff = (T)((SIGN) >> k);                                    \
    unsigned bad = 0;                                                          \
    for (size_t i = 0; i < nb_elts; ++i) {                                     \
      const T prim = p[i];                                                     \
      const T sec = q[i];                                                      \
      bad |= (unsigned)((sec >> k) != 0u);                                     \
      const T low = (prim >= sign_cutoff) ? sec : (T)(maxlow - sec);           \
      const T lat = (T)((prim << k) + low);                                    \
      d[i] = (lat & (SIGN)) ? (T)(lat ^ (SIGN)) : (T)~lat;                     \
    }                                                                          \
    return (int)bad;                                                           \
  } while (0)

int floatquant_join(void *dst, const void *primary, const void *secondary,
                    size_t nb_elts, size_t elt_width, unsigned k) {
  switch (elt_width) {
  case 4:
    FLOATQUANT_JOIN(uint32_t, (uint32_t)0x80000000u);
  case 8:
    FLOATQUANT_JOIN(uint64_t, (uint64_t)0x8000000000000000ull);
  default:
    return 1;
  }
}
