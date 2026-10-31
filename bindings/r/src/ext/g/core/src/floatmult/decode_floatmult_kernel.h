// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pcodec's FloatMult mode. The latent mapping lives in
// floatmult_common.h. The port is partial and geozl does not reproduce
// pcodec's wire format.

#ifndef GEOZL_CODECS_FLOATMULT_DECODE_KERNEL_H
#define GEOZL_CODECS_FLOATMULT_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Rebuild the float stream from the mult and adjustment streams against a float
// base. dst holds nb_elts elements of elt_width bytes. elt_width must be 4
// or 8. The transform is bit exact for every float. There is no per element
// validity check, every pair decodes to a deterministic float, corruption is
// caught by the frame checksum. Returns 0 on success, nonzero only for an
// unsupported width.
int floatmult_join(void *dst, const void *primary, const void *secondary,
                   size_t nb_elts, size_t elt_width, double base);

#endif // GEOZL_CODECS_FLOATMULT_DECODE_KERNEL_H
