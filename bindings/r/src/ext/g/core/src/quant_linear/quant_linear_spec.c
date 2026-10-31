#include "quant_linear_spec.h"

#include "quant_linear_dtype.h"
#include "quant_linear_half.h"

#include "common/recipe_parse.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int quant_linear_parse(const char *s, quant_linear_spec *out, char *err,
                       size_t errSize) {
  memset(out, 0, sizeof(*out));

  if (s == NULL || strncmp(s, "LINEAR:", 7) != 0)
    return geozl_recipe_fail(err, errSize,
                "quant_linear takes \"LINEAR:MAX_ERROR=V\", got \"%s\"",
                s == NULL ? "" : s);

  const char *cur = s + 7;
  const char *vb, *ve;
  int haveError = 0;

  for (;;) {
    if (geozl_recipe_keyed(&cur, "MAX_ERROR", &vb, &ve) == 0) {
      if (haveError || geozl_recipe_number(vb, ve, &out->max_error))
        return geozl_recipe_fail(err, errSize, "error \"%s\": MAX_ERROR takes one number", s);
      if (out->max_error <= 0.0)
        return geozl_recipe_fail(err, errSize, "error \"%s\": MAX_ERROR must be positive", s);
      haveError = 1;
    } else if (geozl_recipe_keyed(&cur, "STORE", &vb, &ve) == 0) {
      return geozl_recipe_fail(err, errSize,
                  "error \"%s\": STORE was removed, a LINEAR stream always "
                  "carries the grid index", s);
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

int quant_linear_resolve(const quant_linear_spec *sp, int dtype,
                         const quant_linear_stats *sc, quant_linear_params *out,
                         char *err, size_t errSize) {
  memset(out, 0, sizeof(*out));
  if (!QL_DTYPE_OK(dtype))
    return geozl_recipe_fail(err, errSize, "dtype %d is not a type this codec knows", dtype);
  const double maxAbs = sc->maxAbs;
  if (!sc->anyNegative)
    out->flags |= QUANT_LINEAR_FLAG_NONNEGATIVE;

  // Integers keep a whole step, so the grid never reads the raster.
  if (dtype <= QL_LAST_INT) {
    out->step = floor(2.0 * sp->max_error);
    if (!isfinite(out->step))
      return geozl_recipe_fail(err, errSize,
                  "a MAX_ERROR of %g makes the grid step too large",
                  sp->max_error);
    if (!(out->step >= 1.0))
      out->step = 1.0; // a step of one is lossless, which is the floor
    // Integer division keeps |index| <= |sample|, including past 2^53.
    return 0;
  }

  // Two roundings, not one. The division inside nearbyint(x/step) can send q to
  // the neighbour the exact quotient would not have picked, which costs eps*|x|,
  // and storing q*step at the output width rounds again by eps*|x^|. See
  // spec.md. This is also the only thing on this path that reads the tile.
  out->step = 2.0 * (sp->max_error - 2.0 * quant_linear_eps(dtype) * maxAbs);
  if (!isfinite(out->step))
    return geozl_recipe_fail(err, errSize,
                "a MAX_ERROR of %g makes the grid step too large",
                sp->max_error);
  if (!(out->step > 0.0))
    return geozl_recipe_fail(err, errSize,
                "a MAX_ERROR of %g is at or below the rounding of the output "
                "type at %g",
                sp->max_error, maxAbs);
  if (maxAbs / out->step > quant_linear_stream_max(dtype))
    return geozl_recipe_fail(err, errSize,
                "a MAX_ERROR of %g needs more levels than a %zu-byte stream "
                "carries",
                sp->max_error, quant_linear_width(dtype));
  return 0;
}

// Worst error as a fraction of the declared bound, so at or under one holds. A
// non-finite source sample has no index and belongs to nodata.
#define QL_VER(RA, RB)                                                         \
  do {                                                                         \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const double x = (double)(RA), y = (double)(RB);                         \
      if (!isfinite(x) || x == y)                                              \
        continue;                                                              \
      const double r = fabs(x - y) / sp->max_error;                            \
      if (r > w)                                                               \
        w = r;                                                                 \
    }                                                                          \
  } while (0)

int quant_linear_verify(const void *src, const void *dec,
                        const quant_linear_spec *sp, int dtype, size_t nbElts,
                        double *worst) {
  double w = 0.0;
  if (src == NULL || dec == NULL || sp == NULL || !QL_DTYPE_OK(dtype) ||
      !(sp->max_error > 0.0))
    return 1;

  switch ((ql_dtype)dtype) {
  case QL_U8:
    QL_VER(((const uint8_t *)src)[i], ((const uint8_t *)dec)[i]);
    break;
  case QL_U16:
    QL_VER(((const uint16_t *)src)[i], ((const uint16_t *)dec)[i]);
    break;
  case QL_U32:
    QL_VER(((const uint32_t *)src)[i], ((const uint32_t *)dec)[i]);
    break;
  case QL_U64:
    QL_VER(((const uint64_t *)src)[i], ((const uint64_t *)dec)[i]);
    break;
  case QL_I8:
    QL_VER(((const int8_t *)src)[i], ((const int8_t *)dec)[i]);
    break;
  case QL_I16:
    QL_VER(((const int16_t *)src)[i], ((const int16_t *)dec)[i]);
    break;
  case QL_I32:
    QL_VER(((const int32_t *)src)[i], ((const int32_t *)dec)[i]);
    break;
  case QL_I64:
    QL_VER(((const int64_t *)src)[i], ((const int64_t *)dec)[i]);
    break;
  case QL_F16:
    QL_VER(quant_linear_half_to_float(((const uint16_t *)src)[i]),
           quant_linear_half_to_float(((const uint16_t *)dec)[i]));
    break;
  case QL_F32:
    QL_VER(((const float *)src)[i], ((const float *)dec)[i]);
    break;
  default:
    QL_VER(((const double *)src)[i], ((const double *)dec)[i]);
    break;
  }

  if (worst != NULL)
    *worst = w;
  return 0;
}
