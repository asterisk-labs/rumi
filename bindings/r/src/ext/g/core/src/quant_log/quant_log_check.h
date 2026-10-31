// What a parameter block has to satisfy, read by both ends and by the binding.
// Lived inside quant_log_dtype.h before, which is not where the other two
// families keep it.

#ifndef GEOZL_CODECS_QUANT_LOG_CHECK_H
#define GEOZL_CODECS_QUANT_LOG_CHECK_H

#include "quant_log_dtype.h"

#include "geozl/quant_log_params.h"

// What a frame carries that a kernel can check without seeing the tile. A step
// that is merely positive is not enough: an infinite one leaves the quotient
// inside exp2 a nan, and the conversion that follows is undefined and lands
// somewhere different on x86 and on arm64.
static inline int quant_log_params_ok(const quant_log_params *p, int dtype) {
  if (p == NULL || !QLOG_DTYPE_OK(dtype))
    return 0;
  if (!(p->step > 0.0) || !(p->step <= QLOG_FINITE_TOP))
    return 0;
  if ((p->flags & ~(unsigned)QUANT_LOG_FLAGS_KNOWN) != 0)
    return 0;
  // An integer type carries the reconstruction and nothing else.
  return dtype > QLOG_LAST_INT || (p->flags & QUANT_LOG_FLAG_STORE_VALUES) != 0;
}

#endif // GEOZL_CODECS_QUANT_LOG_CHECK_H
