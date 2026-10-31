#include "quant_sqrt_spec.h"

#include "quant_sqrt_dtype.h"
#include "quant_sqrt_half.h"

#include "common/recipe_parse.h"

#include <errno.h>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Terms a reconstruction picks up when the decoder rebuilds in float rather than
// double. The step rounds, the index converts, the product rounds, the square
// doubles that, the subtraction rounds again. Eight is a loose count, measured
// worst is near a third of it.
#define QSQ_F32_TERMS 8.0

// Share of the budget the float path may give up before the double path wins.
#define QSQ_F32_SHARE 0.25

// Half ulps the quotient that finds the level picks up, which moves the boundary
// between two levels and costs the step eta*u. Six covers the add, the square,
// the reciprocal, the product and the two the root sits between; measured worst
// is near a third of that. Derived in spec.md under Cutting the step. Leaving it
// out let an f64 grid run 1.40x over its declared bound.
#define QSQ_INDEX_TERMS 3.0

int quant_sqrt_parse(const char *s, quant_sqrt_spec *out, char *err,
                     size_t errSize) {
  memset(out, 0, sizeof(*out));

  if (s == NULL || strncmp(s, "SQRT:", 5) != 0)
    return geozl_recipe_fail(err, errSize,
                "quant_sqrt takes \"SQRT:MAX_ERROR=VN\", got \"%s\"",
                s == NULL ? "" : s);

  const char *cur = s + 5;
  const char *vb, *ve;
  int haveError = 0, haveA = 0, haveB = 0;

  for (;;) {
    if (geozl_recipe_keyed(&cur, "MAX_ERROR", &vb, &ve) == 0) {
      // Without the N a MAX_ERROR of 0.5 reads as half a count, which is also a
      // plausible thing to mean and quantizes a hundred times finer.
      double v;
      if (haveError || ve == vb || ve[-1] != 'N' || geozl_recipe_number(vb, ve - 1, &v))
        return geozl_recipe_fail(err, errSize,
                    "error \"%s\": MAX_ERROR takes one number ending in N, "
                    "which counts sigmas of noise",
                    s);
      if (!(v > 0.0))
        return geozl_recipe_fail(err, errSize, "error \"%s\": MAX_ERROR must be positive", s);
      out->k = v;
      haveError = 1;
    } else if (geozl_recipe_keyed(&cur, "A", &vb, &ve) == 0) {
      if (haveA || geozl_recipe_number(vb, ve, &out->a))
        return geozl_recipe_fail(err, errSize, "error \"%s\": A takes one number", s);
      if (!(out->a >= 0.0))
        return geozl_recipe_fail(err, errSize,
                    "error \"%s\": A is a variance at zero signal and cannot be "
                    "negative",
                    s);
      haveA = 1;
    } else if (geozl_recipe_keyed(&cur, "B", &vb, &ve) == 0) {
      if (haveB || geozl_recipe_number(vb, ve, &out->b))
        return geozl_recipe_fail(err, errSize, "error \"%s\": B takes one number", s);
      if (!(out->b > 0.0))
        return geozl_recipe_fail(err, errSize,
                    "error \"%s\": B must be positive; with B at zero the noise "
                    "does not grow with the signal and this curve is flat",
                    s);
      haveB = 1;
    } else if (geozl_recipe_keyed(&cur, "STORE", &vb, &ve) == 0) {
      return geozl_recipe_fail(err, errSize,
                  "error \"%s\": STORE was removed, a SQRT stream carries the "
                  "grid index wherever it fits the element", s);
    } else {
      return geozl_recipe_fail(err, errSize,
                  "error \"%s\": unknown key, expected MAX_ERROR, A or B", s);
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
  if (haveA != haveB)
    return geozl_recipe_fail(err, errSize,
                "error \"%s\": A and B travel together; give both or neither "
                "and let the fit find them",
                s);
  out->have_ab = (unsigned char)haveA;
  return 0;
}

// The curve, wherever it came from.
static int curve_of(const quant_sqrt_spec *sp, const quant_sqrt_noise *ft,
                    double *a, double *b) {
  if (sp->have_ab) {
    *a = sp->a;
    *b = sp->b;
    return 0;
  }
  if (ft == NULL || !ft->ok || !(ft->b > 0.0))
    return 1;
  *a = ft->a < 0.0 ? 0.0 : ft->a;
  *b = ft->b;
  return 0;
}

// Signed x. The variance grows with the signal, not with its magnitude, and
// folding the curve at zero reports a bound the grid was never cut against.
double quant_sqrt_bound(const quant_sqrt_spec *sp, const quant_sqrt_noise *ft,
                        double x) {
  double a, b;
  if (curve_of(sp, ft, &a, &b) != 0)
    return 0.0;
  const double v = a + b * x;
  return v > 0.0 ? sp->k * sqrt(v) : 0.0;
}

// What rounding a stored reconstruction costs against the bound where the sample
// sits. Grows with x above zero, and runs away as x falls toward -offset where
// the bound goes to zero and the rounding does not, so both ends get measured.
// Zero is exact in every type this codec writes.
static double store_charge(double x, double offset) {
  if (x == 0.0)
    return 0.0;
  const double u = sqrt(x + offset);
  return u > 0.0 ? fabs(x) / u : INFINITY;
}

int quant_sqrt_resolve(const quant_sqrt_spec *sp, int dtype,
                       const quant_sqrt_stats *sc, const quant_sqrt_noise *ft,
                       quant_sqrt_params *out, char *err, size_t errSize) {
  memset(out, 0, sizeof(*out));
  if (!QSQ_DTYPE_OK(dtype))
    return geozl_recipe_fail(err, errSize, "dtype %d is not a type this codec knows", dtype);
  if (!(sp->k > 0.0))
    return geozl_recipe_fail(err, errSize, "MAX_ERROR must be positive");

  double a, b;
  if (curve_of(sp, ft, &a, &b) != 0) {
    if (ft == NULL)
      return geozl_recipe_fail(err, errSize,
                  "this recipe carries no A and B, so it needs a curve from "
                  "quant_sqrt_fit");
    return geozl_recipe_fail(err, errSize,
                "the fit did not hold: %d blocks over %d bins, range %.1fx, "
                "collinearity %.1f, residual %.3f",
                ft->blocks, ft->bins, ft->range, ft->colin, ft->resid);
  }

  // k*sqrt(a + b*x) is k*sqrt(b) * sqrt(x + a/b), so the whole curve is two
  // numbers and those two are what the header carries.
  const double c = sp->k * sqrt(b);
  const double offset = a / b;
  if (!isfinite(c) || !(c > 0.0) || !isfinite(offset) || !(offset >= 0.0))
    return geozl_recipe_fail(err, errSize,
                "a MAX_ERROR of %g with A=%g and B=%g does not give a curve",
                sp->k, a, b);
  out->offset = offset;

  // Nothing below -offset has a bound, since the model puts the variance there
  // at or under zero.
  if (sc->lo < -offset)
    return geozl_recipe_fail(err, errSize,
                "a shot bound is defined at or above -A/B, which is %g, and "
                "this raster reaches %g",
                -offset, sc->lo);
  if (!sc->anyNegative)
    out->flags |= QUANT_SQRT_FLAG_NONNEGATIVE;

  const double maxAbs = fmax(fabs(sc->lo), fabs(sc->hi));
  const double u = sqrt(maxAbs + offset);

  if (dtype <= QSQ_LAST_INT) {
    // Half the budget to the grid and half to rounding the level to a whole
    // number, which meet exactly at sqrt(x+offset) = 1/(2*step).
    //
    // Nothing here reads the raster, on purpose. Two tiles of one product have to
    // land on the same grid or the same value encodes differently in each.
    out->step = 0.5 * c;

    // Refuse grids whose index arithmetic cannot resolve every level.
    const double levels = 0.5 / (QSQ_INDEX_TERMS * DBL_EPSILON);
    if (!(u / out->step < levels))
      return geozl_recipe_fail(err, errSize,
                  "this bound needs %g levels over this raster, past the %g the "
                  "arithmetic that finds one resolves",
                  u / out->step, levels);

    // Fall back to reconstructions when the largest index does not fit.
    const double uTop = sqrt(fmax(sc->hi, -offset) + offset);
    if (!(uTop / out->step < quant_sqrt_stream_max(dtype)))
      out->flags |= QUANT_SQRT_FLAG_STORE_VALUES;
    return 0;
  }

  // The index path, where the step reads the raster anyway, so both charges land
  // on it rather than becoming a refusal the way the value path does it.
  const double eps = quant_sqrt_eps(dtype);
  const double chargeWide =
      eps * fmax(store_charge(sc->lo, offset), store_charge(sc->hi, offset));
  const double chargeIndex = QSQ_INDEX_TERMS * DBL_EPSILON * u;

  double charge = chargeWide;
  if (dtype == QSQ_F32) {
    const double chargeNarrow = QSQ_F32_TERMS * eps * u;
    if (chargeNarrow <= QSQ_F32_SHARE * c) {
      charge = chargeNarrow;
      out->flags |= QUANT_SQRT_FLAG_DECODE_F32;
    }
  }

  out->step = c - charge - chargeIndex;
  if (!(out->step > 0.0))
    return geozl_recipe_fail(err, errSize,
                "this bound is at or below the rounding of the output type "
                "between %g and %g, where the type resolves %g and the bound "
                "asks for %g",
                sc->lo, sc->hi, chargeWide, c);

  const double q = u / out->step;
  if (!(q < quant_sqrt_stream_max(dtype)))
    return geozl_recipe_fail(err, errSize,
                "this bound needs more levels than a %zu-byte index carries",
                quant_sqrt_width(dtype));
  return 0;
}

// An exact sample satisfies any bound and is skipped.
#define QSQ_VER(RA, RB)                                                        \
  do {                                                                         \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const double x = (double)(RA), y = (double)(RB);                         \
      if (!isfinite(x) || x == y)                                              \
        continue;                                                              \
      const double v = a + b * x;                                              \
      if (!(v > 0.0))                                                          \
        continue;                                                              \
      const double r = fabs(x - y) / (sp->k * sqrt(v));                        \
      if (r > w)                                                               \
        w = r;                                                                 \
    }                                                                          \
  } while (0)

int quant_sqrt_verify(const void *src, const void *dec,
                      const quant_sqrt_spec *sp, const quant_sqrt_noise *ft,
                      int dtype, size_t nbElts, double *worst) {
  double w = 0.0, a, b;
  if (!QSQ_DTYPE_OK(dtype) || !(sp->k > 0.0) || curve_of(sp, ft, &a, &b) != 0)
    return 1;

  switch ((qsq_dtype)dtype) {
  case QSQ_U8:
    QSQ_VER(((const uint8_t *)src)[i], ((const uint8_t *)dec)[i]);
    break;
  case QSQ_U16:
    QSQ_VER(((const uint16_t *)src)[i], ((const uint16_t *)dec)[i]);
    break;
  case QSQ_U32:
    QSQ_VER(((const uint32_t *)src)[i], ((const uint32_t *)dec)[i]);
    break;
  case QSQ_U64:
    QSQ_VER(((const uint64_t *)src)[i], ((const uint64_t *)dec)[i]);
    break;
  case QSQ_I8:
    QSQ_VER(((const int8_t *)src)[i], ((const int8_t *)dec)[i]);
    break;
  case QSQ_I16:
    QSQ_VER(((const int16_t *)src)[i], ((const int16_t *)dec)[i]);
    break;
  case QSQ_I32:
    QSQ_VER(((const int32_t *)src)[i], ((const int32_t *)dec)[i]);
    break;
  case QSQ_I64:
    QSQ_VER(((const int64_t *)src)[i], ((const int64_t *)dec)[i]);
    break;
  case QSQ_F16:
    QSQ_VER(quant_sqrt_half_to_float(((const uint16_t *)src)[i]),
            quant_sqrt_half_to_float(((const uint16_t *)dec)[i]));
    break;
  case QSQ_F32:
    QSQ_VER(((const float *)src)[i], ((const float *)dec)[i]);
    break;
  default:
    QSQ_VER(((const double *)src)[i], ((const double *)dec)[i]);
    break;
  }

  if (worst != NULL)
    *worst = w;
  return 0;
}
