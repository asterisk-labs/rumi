#include "lossy_recipe.h"

#include "quant_linear/encode_quant_linear_kernel.h"
#include "quant_linear/quant_linear_half.h"
#include "quant_linear/quant_linear_spec.h"
#include "quant_log/encode_quant_log_kernel.h"
#include "quant_log/quant_log_spec.h"
#include "quant_sqrt/encode_quant_sqrt_kernel.h"
#include "quant_sqrt/quant_sqrt_fit.h"
#include "quant_sqrt/quant_sqrt_spec.h"

#include "common/recipe_parse.h"
#include "geozl/dtype.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// family lands last, so a recipe that did not parse stays NONE and reads as
// lossless rather than as a family sitting over an uninitialised spec.
int geozl_lossy_parse(const char *s, geozl_lossy_recipe *out, char *err,
                      size_t errSize) {
  if (out == NULL)
    return geozl_recipe_fail(err, errSize, "no recipe to write to");
  memset(out, 0, sizeof(*out));
  if (s == NULL || s[0] == '\0')
    return 0;

  if (strncmp(s, "LINEAR:", 7) == 0) {
    if (quant_linear_parse(s, &out->as.linear, err, errSize) != 0)
      return 1;
    out->family = GEOZL_LOSSY_LINEAR;
    return 0;
  }
  if (strncmp(s, "LOG:", 4) == 0) {
    if (quant_log_parse(s, &out->as.log, err, errSize) != 0)
      return 1;
    out->family = GEOZL_LOSSY_LOG;
    return 0;
  }
  if (strncmp(s, "SQRT:", 5) == 0) {
    if (quant_sqrt_parse(s, &out->as.sqrt, err, errSize) != 0)
      return 1;
    out->family = GEOZL_LOSSY_SQRT;
    return 0;
  }
  return geozl_recipe_fail(err, errSize,
              "error \"%s\": the families are LINEAR, LOG and SQRT, each "
              "followed by a colon",
              s);
}

int geozl_lossy_fit(geozl_lossy_recipe *r, const void *src, int dtype,
                    size_t width, size_t nbElts, char *err, size_t errSize) {
  if (r == NULL)
    return geozl_recipe_fail(err, errSize, "no recipe to fit");
  if (r->family != GEOZL_LOSSY_SQRT || r->as.sqrt.have_ab)
    return 0;
  if (width == 0 || nbElts % width != 0)
    return geozl_recipe_fail(err, errSize, "%zu samples do not divide into rows of %zu",
                nbElts, width);

  quant_sqrt_noise curve;
  if (quant_sqrt_fit(src, dtype, width, nbElts / width, &curve, err, errSize) !=
      0)
    return 1;
  r->as.sqrt.a = curve.a;
  r->as.sqrt.b = curve.b;
  r->as.sqrt.have_ab = 1;
  return 0;
}

int geozl_lossy_resolve(const geozl_lossy_recipe *r, const void *src, int dtype,
                        size_t nbElts, geozl_lossy_plan *out, char *err,
                        size_t errSize) {
  if (r == NULL || out == NULL)
    return geozl_recipe_fail(err, errSize, "no recipe to resolve");
  memset(out, 0, sizeof(*out));
  out->family = r->family;

  switch (r->family) {
  case GEOZL_LOSSY_NONE:
    return 0;

  case GEOZL_LOSSY_LINEAR: {
    if (quant_linear_scan(src, dtype, nbElts, &out->domain.linear) != 0)
      return geozl_recipe_fail(err, errSize,
                  "the raster holds no finite sample to cut a grid against");
    return quant_linear_resolve(&r->as.linear, dtype, &out->domain.linear,
                                &out->as.linear, err, errSize);
  }

  case GEOZL_LOSSY_LOG: {
    if (quant_log_scan(src, dtype, nbElts, &out->domain.log) != 0)
      return geozl_recipe_fail(err, errSize, "dtype %d is not a type quant_log knows", dtype);
    return quant_log_resolve(&r->as.log, dtype, &out->domain.log, &out->as.log,
                             err, errSize);
  }

  case GEOZL_LOSSY_SQRT: {
    if (quant_sqrt_scan(src, dtype, nbElts, &out->domain.sqrt) != 0)
      return geozl_recipe_fail(err, errSize, "the raster holds no finite sample");
    // NULL curve: geozl_lossy_fit has already written one into the recipe, or
    // the recipe came with A and B, or quant_sqrt refuses and says so.
    return quant_sqrt_resolve(&r->as.sqrt, dtype, &out->domain.sqrt, NULL,
                              &out->as.sqrt, err, errSize);
  }
  }

  return geozl_recipe_fail(err, errSize, "recipe family %d is not one this build knows",
              (int)r->family);
}

static int rejects_negative(unsigned char flags, unsigned char nonnegative,
                            int anyNegative, char *err, size_t errSize) {
  if ((flags & nonnegative) == 0 || !anyNegative)
    return 0;
  return geozl_recipe_fail(
      err, errSize,
      "the lossy graph was built without negative samples, but this tile "
      "contains them; build the graph from the full product");
}

typedef struct {
  double lo;
  double hi;
  double minAbs;
  double maxAbs;
  int anyNegative;
  int haveFinite;
} geozl_domain_stats;

#define DOMAIN_SCAN(T, U, RD)                                                  \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    for (size_t i = 0; i < nbElts; ++i) {                                     \
      U raw;                                                                   \
      memcpy(&raw, &s[i], sizeof(raw));                                        \
      if (raw == (U)ignoredBits)                                               \
        continue;                                                              \
      const double v = (double)(RD);                                           \
      if (!isfinite(v))                                                        \
        continue;                                                              \
      out->haveFinite = 1;                                                     \
      if (v < out->lo)                                                         \
        out->lo = v;                                                           \
      if (v > out->hi)                                                         \
        out->hi = v;                                                           \
      if (v < 0.0)                                                            \
        out->anyNegative = 1;                                                  \
      const double a = fabs(v);                                                \
      if (a > out->maxAbs)                                                     \
        out->maxAbs = a;                                                       \
      if (a > 0.0 && a < out->minAbs)                                          \
        out->minAbs = a;                                                       \
    }                                                                          \
  } while (0)

// The nodata node removes a declared sentinel before the quantizer sees it. A
// regular scan would mistake an outlying sentinel for an outlying sample.
static int scan_ignoring_value(const void *src, int dtype, size_t nbElts,
                               uint64_t ignoredBits,
                               geozl_domain_stats *out) {
  if (src == NULL || out == NULL || !GEOZL_DT_OK(dtype))
    return 1;
  out->lo = INFINITY;
  out->hi = -INFINITY;
  out->minAbs = INFINITY;
  out->maxAbs = 0.0;
  out->anyNegative = 0;
  out->haveFinite = 0;

  switch ((geozl_dtype)dtype) {
  case GEOZL_DT_U8:
    DOMAIN_SCAN(uint8_t, uint8_t, s[i]);
    break;
  case GEOZL_DT_U16:
    DOMAIN_SCAN(uint16_t, uint16_t, s[i]);
    break;
  case GEOZL_DT_U32:
    DOMAIN_SCAN(uint32_t, uint32_t, s[i]);
    break;
  case GEOZL_DT_U64:
    DOMAIN_SCAN(uint64_t, uint64_t, s[i]);
    break;
  case GEOZL_DT_I8:
    DOMAIN_SCAN(int8_t, uint8_t, s[i]);
    break;
  case GEOZL_DT_I16:
    DOMAIN_SCAN(int16_t, uint16_t, s[i]);
    break;
  case GEOZL_DT_I32:
    DOMAIN_SCAN(int32_t, uint32_t, s[i]);
    break;
  case GEOZL_DT_I64:
    DOMAIN_SCAN(int64_t, uint64_t, s[i]);
    break;
  case GEOZL_DT_F16:
    DOMAIN_SCAN(uint16_t, uint16_t, quant_linear_half_to_float(s[i]));
    break;
  case GEOZL_DT_F32:
    DOMAIN_SCAN(float, uint32_t, s[i]);
    break;
  case GEOZL_DT_F64:
    DOMAIN_SCAN(double, uint64_t, s[i]);
    break;
  }
  return 0;
}

#undef DOMAIN_SCAN

int geozl_lossy_check_domain(const geozl_lossy_plan *plan, const void *src,
                             int dtype, size_t nbElts, int ignoreValue,
                             uint64_t ignoredBits, char *err, size_t errSize) {
  if (plan == NULL || !GEOZL_DT_OK(dtype) || (src == NULL && nbElts != 0))
    return geozl_recipe_fail(err, errSize, "invalid lossy domain check");
  if (plan->family == GEOZL_LOSSY_NONE || nbElts == 0)
    return 0;

  switch (plan->family) {
  case GEOZL_LOSSY_LINEAR: {
    quant_linear_stats now = {0};
    if (ignoreValue) {
      geozl_domain_stats sc;
      if (scan_ignoring_value(src, dtype, nbElts, ignoredBits, &sc) != 0)
        return geozl_recipe_fail(err, errSize, "could not scan the lossy tile");
      now.maxAbs = sc.maxAbs;
      now.anyNegative = sc.anyNegative;
    } else {
      // An all-zero tile is valid even though the resolver cannot cut a new
      // grid from one. The scan still returns its zeroed statistics.
      (void)quant_linear_scan(src, dtype, nbElts, &now);
    }
    if (rejects_negative(plan->as.linear.flags,
                         QUANT_LINEAR_FLAG_NONNEGATIVE, now.anyNegative, err,
                         errSize) != 0)
      return 1;
    if (dtype > GEOZL_DT_LAST_INT &&
        now.maxAbs > plan->domain.linear.maxAbs)
      return geozl_recipe_fail(
          err, errSize,
          "the lossy graph was built for magnitudes up to %g, but this tile "
          "reaches %g; build the graph from the full product",
          plan->domain.linear.maxAbs, now.maxAbs);
    return 0;
  }

  case GEOZL_LOSSY_LOG: {
    quant_log_stats now;
    if (ignoreValue) {
      geozl_domain_stats sc;
      if (scan_ignoring_value(src, dtype, nbElts, ignoredBits, &sc) != 0)
        return geozl_recipe_fail(err, errSize, "could not scan the lossy tile");
      now.minAbs = sc.minAbs;
      now.maxAbs = sc.maxAbs;
      now.anyNegative = sc.anyNegative;
      now.anySubnormal = 0;
    } else if (quant_log_scan(src, dtype, nbElts, &now) != 0) {
      return geozl_recipe_fail(err, errSize, "could not scan the lossy tile");
    }
    // Every LOG grid spans its whole dtype, so only the sign can leave it.
    return rejects_negative(plan->as.log.flags, QUANT_LOG_FLAG_NONNEGATIVE,
                            now.anyNegative, err, errSize);
  }

  case GEOZL_LOSSY_SQRT: {
    quant_sqrt_stats now;
    if (ignoreValue) {
      geozl_domain_stats sc;
      if (scan_ignoring_value(src, dtype, nbElts, ignoredBits, &sc) != 0)
        return geozl_recipe_fail(err, errSize, "could not scan the lossy tile");
      if (!sc.haveFinite)
        return 0;
      now.lo = sc.lo;
      now.hi = sc.hi;
      now.anyNegative = sc.anyNegative;
      now.anyNonFinite = 0;
    } else if (quant_sqrt_scan(src, dtype, nbElts, &now) != 0) {
      return 0; // no finite sample lies outside the finite fitted domain
    }
    if (now.lo < plan->domain.sqrt.lo || now.hi > plan->domain.sqrt.hi)
      return geozl_recipe_fail(
          err, errSize,
          "the lossy graph was built for values from %g to %g, but this tile "
          "reaches %g to %g; build the graph from the full product",
          plan->domain.sqrt.lo, plan->domain.sqrt.hi, now.lo, now.hi);
    return 0;
  }

  case GEOZL_LOSSY_NONE:
    return 0;
  }

  return geozl_recipe_fail(err, errSize,
                           "lossy family %d is not one this build knows",
                           (int)plan->family);
}

// Largest distance from s that the recipe may reconstruct as s.
double geozl_lossy_guard_radius(const geozl_lossy_recipe *r, double s) {
  switch (r->family) {
  case GEOZL_LOSSY_NONE:
    return 0.0;
  case GEOZL_LOSSY_LINEAR:
    return r->as.linear.max_error >= 0.0 ? r->as.linear.max_error : INFINITY;
  case GEOZL_LOSSY_LOG: {
    // The farther root of |x - s| <= p|x|.
    const double p = r->as.log.rel_err;
    if (!(p > 0.0 && p < 1.0) || !isfinite(s))
      return INFINITY;
    return p * fabs(s) / (1.0 - p);
  }
  case GEOZL_LOSSY_SQRT: {
    // Larger root of t^2 = k^2(a + b(s + t)); the lower side is shorter.
    const quant_sqrt_spec *q = &r->as.sqrt;
    if (!q->have_ab || !isfinite(s) || !(q->b > 0.0) || !(q->k > 0.0))
      return INFINITY;
    const double k2 = q->k * q->k;
    const double disc = k2 * k2 * q->b * q->b + 4.0 * k2 * (q->a + q->b * s);
    if (disc < 0.0)
      return 0.0;
    return 0.5 * (k2 * q->b + sqrt(disc));
  }
  }
  return INFINITY;
}
