#ifndef RUMI_CODECS_QUANT_LINEAR_DECODE_KERNEL_H
#define RUMI_CODECS_QUANT_LINEAR_DECODE_KERNEL_H

#include <stddef.h>

// Rebuild the original typed stream from integer indices, x = q * scale. Not a
// true inverse, the loss happened in encode. dtype is the ql_dtype of the output,
// scale = 2 * max_error. dst holds nbElts of the output element width.
void quant_linear_decode(void* dst, const void* src,
                         double scale, int dtype, size_t nbElts);

#endif // RUMI_CODECS_QUANT_LINEAR_DECODE_KERNEL_H
