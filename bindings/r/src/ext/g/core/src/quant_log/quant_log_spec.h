#ifndef GEOZL_CODECS_QUANT_LOG_SPEC_H
#define GEOZL_CODECS_QUANT_LOG_SPEC_H

#include "geozl/quant_log_params.h"

#include <stddef.h>

// Parse "LOG:MAX_ERROR=P%".
int quant_log_parse(const char *s, quant_log_spec *out, char *err,
                    size_t errSize);

// Resolve a parsed recipe using statistics from quant_log_scan.
int quant_log_resolve(const quant_log_spec *sp, int dtype,
                      const quant_log_stats *sc, quant_log_params *out,
                      char *err, size_t errSize);

// Verify the error bound. worst <= 1 means the bound holds. skipped counts
// floating-point samples too small to represent a relative bound.
int quant_log_verify(const void *src, const void *dec, const quant_log_spec *sp,
                     int dtype, size_t nbElts, double *worst, size_t *skipped);

#endif // GEOZL_CODECS_QUANT_LOG_SPEC_H
