// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pcodec's FloatQuant mode and the ordered latent mapping
// in pco/src/data_types/float.rs. The port is partial and geozl does not
// reproduce pcodec's wire format.

#ifndef GEOZL_CODECS_FLOATQUANT_DECODE_KERNEL_H
#define GEOZL_CODECS_FLOATQUANT_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Rebuild the float stream from the high part and the low k mantissa bits.
// dst holds nb_elts elements of elt_width bytes. elt_width must be 4 or 8. k
// must be in 1..=PRECISION_BITS. Returns 0 on success, nonzero if any secondary
// is not a k-bit value, the only corruption the pairing can detect. On nonzero
// dst holds garbage but every access stayed in bounds.
int floatquant_join(void *dst, const void *primary, const void *secondary,
                    size_t nb_elts, size_t elt_width, unsigned k);

#endif // GEOZL_CODECS_FLOATQUANT_DECODE_KERNEL_H
