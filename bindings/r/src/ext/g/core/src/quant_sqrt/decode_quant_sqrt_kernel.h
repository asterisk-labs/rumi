#ifndef GEOZL_CODECS_QUANT_SQRT_DECODE_KERNEL_H
#define GEOZL_CODECS_QUANT_SQRT_DECODE_KERNEL_H

#include "geozl/quant_sqrt_params.h"

#include <stddef.h>

// Stream back into samples. dst holds nbElts elements at the width of dtype.
// Returns nonzero on parameters no frame this encoder wrote would carry.
int quant_sqrt_decode(void *restrict dst, const void *restrict src,
                      const quant_sqrt_params *p, int dtype, size_t nbElts);

#endif // GEOZL_CODECS_QUANT_SQRT_DECODE_KERNEL_H
