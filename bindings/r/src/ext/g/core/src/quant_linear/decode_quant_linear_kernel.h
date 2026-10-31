#ifndef GEOZL_CODECS_QUANT_LINEAR_DECODE_KERNEL_H
#define GEOZL_CODECS_QUANT_LINEAR_DECODE_KERNEL_H

#include "geozl/quant_linear_params.h"

#include <stddef.h>

// Rebuild the original stream. x = q * step, or a copy where the stream already
// holds the reconstruction, clamped into the output range and floored at zero when
// QUANT_LINEAR_FLAG_NONNEGATIVE is set. dst must hold nbElts elements of the width
// dtype names. Returns 0, or nonzero for a header only a damaged frame carries.
int quant_linear_decode(void *restrict dst, const void *restrict src,
                        const quant_linear_params *p, int dtype, size_t nbElts);

#endif // GEOZL_CODECS_QUANT_LINEAR_DECODE_KERNEL_H
