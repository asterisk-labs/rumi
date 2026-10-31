#ifndef GEOZL_QUANT_LINEAR_PARAMS_H
#define GEOZL_QUANT_LINEAR_PARAMS_H

// Clamp reconstruction at zero when the input has no negative samples.
#define QUANT_LINEAR_FLAG_NONNEGATIVE 1u

// Legacy frames may store reconstructed values instead of grid indices.
#define QUANT_LINEAR_FLAG_STORE_VALUES 2u

#define QUANT_LINEAR_FLAGS_KNOWN                                               \
  (QUANT_LINEAR_FLAG_NONNEGATIVE | QUANT_LINEAR_FLAG_STORE_VALUES)

typedef struct {
  unsigned char flags;
  double step;
} quant_linear_params;

// Parsed recipe before it is resolved for a dtype and raster.
typedef struct {
  double max_error;
} quant_linear_spec;

// Statistics used to resolve a recipe.
typedef struct {
  double maxAbs; // largest finite magnitude, or 0
  int anyNegative;
} quant_linear_stats;

#endif // GEOZL_QUANT_LINEAR_PARAMS_H
