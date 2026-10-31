// What a parameter block has to satisfy, read by both ends and by the binding.
// Written three times before, and the three had drifted.

#ifndef GEOZL_CODECS_QUANT_SQRT_CHECK_H
#define GEOZL_CODECS_QUANT_SQRT_CHECK_H

#include "quant_sqrt_dtype.h"

#include "geozl/quant_sqrt_params.h"

#include <float.h>
#include <math.h>

static inline int quant_sqrt_params_ok(const quant_sqrt_params *p, int dtype) {
  if (p == NULL || !QSQ_DTYPE_OK(dtype))
    return 0;
  if ((p->flags & ~(unsigned)QUANT_SQRT_FLAGS_KNOWN) != 0)
    return 0;
  // The encoder divides by the square, so the square is what has to be normal. A
  // step that is only positive and finite can still square to zero, and then the
  // reciprocal is infinity and every sample folds onto one level in silence. The
  // sign is separate, a negative step squares to a normal number.
  if (!isfinite(p->step) || !(p->step > 0.0) || !(p->step * p->step >= DBL_MIN))
    return 0;
  if (!isfinite(p->offset) || !(p->offset >= 0.0))
    return 0;
  // The bit says which arithmetic the step was cut against, and only float32 has
  // two to pick from.
  if ((p->flags & QUANT_SQRT_FLAG_DECODE_F32) != 0 && dtype != QSQ_F32)
    return 0;
  return 1;
}

#endif // GEOZL_CODECS_QUANT_SQRT_CHECK_H
