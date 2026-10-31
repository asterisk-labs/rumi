#include "quant_log_spec.h"

#include "quant_log_dtype.h"
#include "quant_log_half.h"
#include "quant_log_math.h"

#include "common/recipe_parse.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int quant_log_parse(const char *s, quant_log_spec *out, char *err,
                    size_t errSize) {
  memset(out, 0, sizeof(*out));

  if (s == NULL || strncmp(s, "LOG:", 4) != 0)
    return geozl_recipe_fail(err, errSize, "quant_log takes \"LOG:MAX_ERROR=V%%\", got \"%s\"",
                s == NULL ? "" : s);

  const char *cur = s + 4;
  const char *vb, *ve;
  int haveError = 0;

  for (;;) {
    if (geozl_recipe_keyed(&cur, "MAX_ERROR", &vb, &ve) == 0) {
      // The per cent sign is required, or MAX_ERROR=1 reads as a bound of one and
      // the caller almost certainly meant a hundredth of that.
      double v;
      if (haveError || ve == vb || ve[-1] != '%' || geozl_recipe_number(vb, ve - 1, &v))
        return geozl_recipe_fail(err, errSize,
                    "error \"%s\": MAX_ERROR takes one number ending in %%", s);
      if (!(v > 0.0))
        return geozl_recipe_fail(err, errSize, "error \"%s\": MAX_ERROR must be positive", s);
      if (!(v < 100.0))
        return geozl_recipe_fail(err, errSize,
                    "error \"%s\": a MAX_ERROR of %g%% is the value itself", s,
                    v);
      out->rel_err = v / 100.0;
      haveError = 1;
    } else if (geozl_recipe_keyed(&cur, "STORE", &vb, &ve) == 0) {
      return geozl_recipe_fail(err, errSize,
                  "error \"%s\": STORE was removed, a LOG stream carries values "
                  "for integers and grid indices for floats", s);
    } else {
      return geozl_recipe_fail(err, errSize,
                  "error \"%s\": unknown key, expected MAX_ERROR", s);
    }
    if (*cur == '\0')
      break;
    if (*cur != ',' || cur[1] == '\0')
      return geozl_recipe_fail(err, errSize, "error \"%s\": a key has to follow every comma",
                  s);
    ++cur;
  }
  if (!haveError)
    return geozl_recipe_fail(err, errSize, "error \"%s\": MAX_ERROR is required", s);
  return 0;
}

int quant_log_resolve(const quant_log_spec *sp, int dtype,
                      const quant_log_stats *sc, quant_log_params *out,
                      char *err, size_t errSize) {
  memset(out, 0, sizeof(*out));
  if (!QLOG_DTYPE_OK(dtype))
    return geozl_recipe_fail(err, errSize, "dtype %d is not a type this codec knows", dtype);
  if (!(sp->rel_err > 0.0))
    return geozl_recipe_fail(err, errSize, "MAX_ERROR must be positive");

  const double b = sp->rel_err;
  // Integer streams hold the rounded reconstruction.
  const int values = dtype <= QLOG_LAST_INT;

  if (!sc->anyNegative)
    out->flags |= QUANT_LOG_FLAG_NONNEGATIVE;
  if (values)
    out->flags |= QUANT_LOG_FLAG_STORE_VALUES;

  const double slack = quant_log_slack(dtype, values);

  if (values) {
    // Two things stand between the sample and the reconstruction and each is a
    // ratio, so they multiply. The grid, and the rounding of the level to a whole
    // number. Split the budget evenly and each gets sqrt(1+b).
    const double r = sqrt(1.0 + b);
    const double half =
        (log1p(r - 1.0) - log1p(slack * QLOG_LN2)) * QLOG_LOG2E - slack;
    if (!(half > 0.0))
      return geozl_recipe_fail(err, errSize,
                  "a MAX_ERROR of %g%% is too tight for a whole-number "
                  "reconstruction",
                  b * 100.0);
    out->step = 2.0 * half;
    return 0;
  }

  // The index path adds the rounding when the level reaches the output width.
  const double eps = quant_log_eps(dtype);
  const double half =
      (log1p(b) - log1p(eps) - log1p(slack * QLOG_LN2)) * QLOG_LOG2E - slack;
  if (!(half > 0.0))
    return geozl_recipe_fail(err, errSize,
                "a MAX_ERROR of %g%% is at or below what this type rebuilds to, "
                "which bottoms out near %g%%",
                b * 100.0, (eps + slack * QLOG_LN2) * 100.0);
  out->step = 2.0 * half;

  // The whole type has to fit, since the grid never reads the tile and so cannot
  // be cut down to the range that happens to be present.
  if (quant_log_index_top(out->step, dtype) >= quant_log_stream_max(dtype))
    return geozl_recipe_fail(err, errSize,
                "a MAX_ERROR of %g%% needs more levels than a %zu-byte index "
                "carries",
                b * 100.0, quant_log_width(dtype));
  return 0;
}

// Identical satisfies any bound, which is what carries zero and every sample the
// grid rebuilds exactly.
#define QLOG_VER(RA, RB)                                                       \
  do {                                                                         \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const double x = (double)(RA), y = (double)(RB);                         \
      if (!isfinite(x) || x == y)                                              \
        continue;                                                              \
      const double a = fabs(x);                                                \
      if (a < nmin) {                                                          \
        ++skip;                                                                \
        continue;                                                              \
      }                                                                        \
      const double r = fabs(x - y) / (sp->rel_err * a);                        \
      if (r > w)                                                               \
        w = r;                                                                 \
    }                                                                          \
  } while (0)

int quant_log_verify(const void *src, const void *dec, const quant_log_spec *sp,
                     int dtype, size_t nbElts, double *worst, size_t *skipped) {
  double w = 0.0;
  size_t skip = 0;
  if (!QLOG_DTYPE_OK(dtype) || !(sp->rel_err > 0.0))
    return 1;
  const double nmin =
      dtype > QLOG_LAST_INT ? quant_log_normal_min(dtype) : 0.0;

  switch ((qlog_dtype)dtype) {
  case QLOG_U8:
    QLOG_VER(((const uint8_t *)src)[i], ((const uint8_t *)dec)[i]);
    break;
  case QLOG_U16:
    QLOG_VER(((const uint16_t *)src)[i], ((const uint16_t *)dec)[i]);
    break;
  case QLOG_U32:
    QLOG_VER(((const uint32_t *)src)[i], ((const uint32_t *)dec)[i]);
    break;
  case QLOG_U64:
    QLOG_VER(((const uint64_t *)src)[i], ((const uint64_t *)dec)[i]);
    break;
  case QLOG_I8:
    QLOG_VER(((const int8_t *)src)[i], ((const int8_t *)dec)[i]);
    break;
  case QLOG_I16:
    QLOG_VER(((const int16_t *)src)[i], ((const int16_t *)dec)[i]);
    break;
  case QLOG_I32:
    QLOG_VER(((const int32_t *)src)[i], ((const int32_t *)dec)[i]);
    break;
  case QLOG_I64:
    QLOG_VER(((const int64_t *)src)[i], ((const int64_t *)dec)[i]);
    break;
  case QLOG_F16:
    QLOG_VER(quant_log_half_to_float(((const uint16_t *)src)[i]),
             quant_log_half_to_float(((const uint16_t *)dec)[i]));
    break;
  case QLOG_F32:
    QLOG_VER(((const float *)src)[i], ((const float *)dec)[i]);
    break;
  default:
    QLOG_VER(((const double *)src)[i], ((const double *)dec)[i]);
    break;
  }

  if (worst != NULL)
    *worst = w;
  if (skipped != NULL)
    *skipped = skip;
  return 0;
}
