#ifndef GEOZL_QUANT_SQRT_PARAMS_H
#define GEOZL_QUANT_SQRT_PARAMS_H

// Clamp reconstruction at zero when the input has no negative samples.
#define QUANT_SQRT_FLAG_NONNEGATIVE 1u

// Store reconstructed values when the index does not fit, and read legacy frames.
#define QUANT_SQRT_FLAG_STORE_VALUES 2u

// Reconstruct float32 output with float arithmetic.
#define QUANT_SQRT_FLAG_DECODE_F32 4u

#define QUANT_SQRT_FLAGS_KNOWN                                                 \
  (QUANT_SQRT_FLAG_NONNEGATIVE | QUANT_SQRT_FLAG_STORE_VALUES |                \
   QUANT_SQRT_FLAG_DECODE_F32)

// Resolved grid in u = sqrt(x + offset).
typedef struct {
  unsigned char flags;
  double step;
  double offset;
} quant_sqrt_params;

// Parsed recipe. A and B may be supplied by quant_sqrt_fit.
typedef struct {
  double k;
  double a;
  double b;
  unsigned char have_ab;
} quant_sqrt_spec;

// Statistics used to resolve a recipe.
typedef struct {
  double lo;        // smallest sample, signed, +inf when there is none finite
  double hi;        // largest sample, signed, -inf when there is none finite
  int anyNegative;
  int anyNonFinite;
} quant_sqrt_stats;

// Result and diagnostics from fitting variance = a + b*x.
typedef struct {
  double a;
  double b;
  int ok;
  int blocks;      // blocks measured
  int bins;        // intensity bins that survived
  double range;    // mu_max / mu_min over the surviving bins
  double colin;    // larger values mean a and b are harder to separate
  double resid;    // relative RMS residual
} quant_sqrt_noise;

#endif // GEOZL_QUANT_SQRT_PARAMS_H
