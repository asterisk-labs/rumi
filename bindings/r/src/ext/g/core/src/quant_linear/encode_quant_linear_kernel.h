#ifndef GEOZL_CODECS_QUANT_LINEAR_ENCODE_KERNEL_H
#define GEOZL_CODECS_QUANT_LINEAR_ENCODE_KERNEL_H

#include "geozl/quant_linear_params.h"

#include <stddef.h>

// Quantize onto a uniform grid. dst holds nbElts elements at dtype's width.
// Returns nonzero for invalid parameters.
int quant_linear_encode(void *restrict dst, const void *restrict src,
                        const quant_linear_params *p, int dtype, size_t nbElts);

// Scan finite samples for grid resolution. Returns 1 if none are finite.
int quant_linear_scan(const void *src, int dtype, size_t nbElts,
                      quant_linear_stats *out);

#endif // GEOZL_CODECS_QUANT_LINEAR_ENCODE_KERNEL_H
