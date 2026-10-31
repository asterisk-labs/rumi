#ifndef GEOZL_QUANT_LOG_PARAMS_H
#define GEOZL_QUANT_LOG_PARAMS_H

// Clamp reconstruction at zero when the input has no negative samples.
#define QUANT_LOG_FLAG_NONNEGATIVE 1u

// Integer and legacy float frames may store reconstructed values.
#define QUANT_LOG_FLAG_STORE_VALUES 2u

#define QUANT_LOG_FLAGS_KNOWN                                                  \
  (QUANT_LOG_FLAG_NONNEGATIVE | QUANT_LOG_FLAG_STORE_VALUES)

// step is the level width in log2 space.
typedef struct {
  unsigned char flags;
  double step;
} quant_log_params;

// Parsed recipe before it is resolved for a dtype and raster.
typedef struct {
  double rel_err;
} quant_log_spec;

// Statistics used to resolve a recipe.
typedef struct {
  double minAbs;    // smallest non-zero magnitude, or +inf if there is none
  double maxAbs;    // largest finite magnitude, or 0
  int anyNegative;
  int anySubnormal; // float types only
} quant_log_stats;

#endif // GEOZL_QUANT_LOG_PARAMS_H
