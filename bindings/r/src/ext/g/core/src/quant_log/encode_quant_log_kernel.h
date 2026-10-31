#ifndef GEOZL_CODECS_ENCODE_QUANT_LOG_KERNEL_H
#define GEOZL_CODECS_ENCODE_QUANT_LOG_KERNEL_H

#include "geozl/quant_log_params.h"

#include <stddef.h>

// Quantize into dst, which holds nbElts elements at dtype's width. Returns
// nonzero for invalid parameters.
int quant_log_encode(void *restrict dst, const void *restrict src,
                     const quant_log_params *p, int dtype, size_t nbElts);

// Scan samples for grid resolution.
int quant_log_scan(const void *src, int dtype, size_t nbElts,
                   quant_log_stats *out);

#endif // GEOZL_CODECS_ENCODE_QUANT_LOG_KERNEL_H
