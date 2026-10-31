#ifndef GEOZL_CODECS_QUANT_SQRT_ENCODE_KERNEL_H
#define GEOZL_CODECS_QUANT_SQRT_ENCODE_KERNEL_H

#include "geozl/quant_sqrt_params.h"

#include <stddef.h>

// Quantize into dst, which holds nbElts elements at dtype's width. Returns
// nonzero for invalid parameters.
int quant_sqrt_encode(void *restrict dst, const void *restrict src,
                      const quant_sqrt_params *p, int dtype, size_t nbElts);

// Scan finite samples for grid resolution. Returns 1 if none are finite.
int quant_sqrt_scan(const void *src, int dtype, size_t nbElts,
                    quant_sqrt_stats *out);

#endif // GEOZL_CODECS_QUANT_SQRT_ENCODE_KERNEL_H
