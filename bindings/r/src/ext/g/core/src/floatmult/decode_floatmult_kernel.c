// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pcodec's FloatMult mode. The latent mapping lives in
// floatmult_common.h. The port is partial and geozl does not reproduce
// pcodec's wire format.

#include "decode_floatmult_kernel.h"
#include "floatmult_common.h"

#include <stdint.h>
#include <string.h>

int floatmult_join(void *dst, const void *primary, const void *secondary,
                   size_t nb_elts, size_t elt_width, double base) {
  if (elt_width == 4) {
    const float b = (float)base;
    const uint32_t *p = (const uint32_t *)primary;
    const uint32_t *q = (const uint32_t *)secondary;
    uint32_t *d = (uint32_t *)dst;
    for (size_t i = 0; i < nb_elts; ++i) {
      const float mult = fm_int_from_latent32(p[i]);
      const float prod = mult * b;
      uint32_t pb;
      memcpy(&pb, &prod, 4);
      const uint32_t adj = q[i] - 0x80000000u; // untoggle
      const uint32_t xk = fm_ord32(pb) + adj;
      d[i] = fm_unord32(xk);
    }
    return 0;
  } else if (elt_width == 8) {
    const double b = base;
    const uint64_t *p = (const uint64_t *)primary;
    const uint64_t *q = (const uint64_t *)secondary;
    uint64_t *d = (uint64_t *)dst;
    for (size_t i = 0; i < nb_elts; ++i) {
      const double mult = fm_int_from_latent64(p[i]);
      const double prod = mult * b;
      uint64_t pb;
      memcpy(&pb, &prod, 8);
      const uint64_t adj = q[i] - 0x8000000000000000ull; // untoggle
      const uint64_t xk = fm_ord64(pb) + adj;
      d[i] = fm_unord64(xk);
    }
    return 0;
  }
  return 1;
}
