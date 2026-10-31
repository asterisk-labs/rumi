#ifndef GEOZL_CODECS_QUANT_LINEAR_SPEC_H
#define GEOZL_CODECS_QUANT_LINEAR_SPEC_H

#include "geozl/quant_linear_params.h"

#include <stddef.h>

// Parse "LINEAR:MAX_ERROR=V".
int quant_linear_parse(const char *s, quant_linear_spec *out, char *err,
                       size_t errSize);

// Resolve a parsed recipe using statistics from quant_linear_scan.
int quant_linear_resolve(const quant_linear_spec *sp, int dtype,
                         const quant_linear_stats *sc, quant_linear_params *out,
                         char *err, size_t errSize);

// Verify the error bound. worst <= 1 means the bound holds.
int quant_linear_verify(const void *src, const void *dec,
                        const quant_linear_spec *sp, int dtype, size_t nbElts,
                        double *worst);

#endif // GEOZL_CODECS_QUANT_LINEAR_SPEC_H
