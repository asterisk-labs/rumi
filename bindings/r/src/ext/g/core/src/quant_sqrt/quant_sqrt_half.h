// The shared half conversions under this codec's names, so the kernels read the
// same as the other two families.

#ifndef GEOZL_CODECS_QUANT_SQRT_HALF_H
#define GEOZL_CODECS_QUANT_SQRT_HALF_H

#include "common/half.h"

#define quant_sqrt_half_to_float geozl_half_to_float
#define quant_sqrt_float_to_half geozl_float_to_half

#endif // GEOZL_CODECS_QUANT_SQRT_HALF_H
