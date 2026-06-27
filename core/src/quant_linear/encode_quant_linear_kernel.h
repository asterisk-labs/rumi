#ifndef RUMI_CODECS_QUANT_LINEAR_ENCODE_KERNEL_H
#define RUMI_CODECS_QUANT_LINEAR_ENCODE_KERNEL_H

#include <stddef.h>

// Quantize a numeric stream to integer indices, q = round(x / scale). The only
// lossy step. dtype is the ql_dtype of the input, scale = 2 * max_error. The
// index stream has the same element width as the input. dst must hold nbElts of
// that width. See graph_quant_linear.h for dtype.
void quant_linear_encode(void* dst, const void* src,
                         double scale, int dtype, size_t nbElts);

#endif // RUMI_CODECS_QUANT_LINEAR_ENCODE_KERNEL_H
