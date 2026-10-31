#ifndef GEOZL_CODECS_QUANT_SQRT_SPEC_H
#define GEOZL_CODECS_QUANT_SQRT_SPEC_H

#include "geozl/quant_sqrt_params.h"

#include <stddef.h>

// Parse "SQRT:MAX_ERROR=VN[,A=a,B=b]". A and B must appear
// together; otherwise quant_sqrt_resolve requires a fit.
int quant_sqrt_parse(const char *s, quant_sqrt_spec *out, char *err,
                     size_t errSize);

// Resolve a parsed recipe. ft may be NULL when the recipe contains A and B.
int quant_sqrt_resolve(const quant_sqrt_spec *sp, int dtype,
                       const quant_sqrt_stats *sc, const quant_sqrt_noise *ft,
                       quant_sqrt_params *out, char *err, size_t errSize);

// Return the absolute error bound at x.
double quant_sqrt_bound(const quant_sqrt_spec *sp, const quant_sqrt_noise *ft,
                        double x);

// Verify the error bound. worst <= 1 means the bound holds.
int quant_sqrt_verify(const void *src, const void *dec,
                      const quant_sqrt_spec *sp, const quant_sqrt_noise *ft,
                      int dtype, size_t nbElts, double *worst);

#endif // GEOZL_CODECS_QUANT_SQRT_SPEC_H
