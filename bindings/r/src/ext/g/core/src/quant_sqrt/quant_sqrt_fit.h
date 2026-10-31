#ifndef GEOZL_CODECS_QUANT_SQRT_FIT_H
#define GEOZL_CODECS_QUANT_SQRT_FIT_H

#include "geozl/quant_sqrt_params.h"

#include <stddef.h>

// Fit variance = a + b*x from local mean/variance samples. The estimator follows
// Abramova et al., SPIE 10004 (2016). Fit once per product, not once per tile.
int quant_sqrt_fit(const void *src, int dtype, size_t width, size_t height,
                   quant_sqrt_noise *out, char *err, size_t errSize);

// Accumulator for fitting one model across several rasters.
typedef struct {
  double *mu;      // local means
  double *s2;      // local variances, already corrected for the quantile bias
  size_t n;        // pairs held
  size_t cap;      // pairs the buffer holds
  size_t stride;   // one block in every stride is kept once the cap is reached
  size_t seen;     // blocks measured, including the ones dropped
  int failed;      // allocation gave out
} quant_sqrt_accum;

// Initialize, extend and solve an accumulator. free releases its buffers.
void quant_sqrt_accum_init(quant_sqrt_accum *acc);
void quant_sqrt_accum_free(quant_sqrt_accum *acc);
int quant_sqrt_accum_push(quant_sqrt_accum *acc, const void *src, int dtype,
                          size_t width, size_t height);
int quant_sqrt_accum_solve(const quant_sqrt_accum *acc, quant_sqrt_noise *out,
                           char *err, size_t errSize);

#endif // GEOZL_CODECS_QUANT_SQRT_FIT_H
