#include "encode_quant_sqrt_kernel.h"

#include "quant_sqrt_check.h"
#include "quant_sqrt_dtype.h"
#include "quant_sqrt_half.h"

#include <math.h>
#include <stdint.h>

// The level nearest the sample in x, not in u. Rounding in the warped domain
// would cost a third of the step and this costs one sqrt either way, derived in
// spec.md under Cutting the step.
//
// The guard is !(t > 0.25) so a NaN takes the zero branch, since casting one to
// an integer is undefined. A raster that means its NaNs carries nodata in front.
#define QSQ_INDEX(x)                                                           \
  ((t = ((double)(x) + offset) * inv), !(t > 0.25)                             \
       ? (int64_t)0                                                            \
       : ((r = 0.5 + sqrt(t - 0.25)), r >= dtop ? top : (int64_t)r))

// Every index is at or above zero, the grid lives over x + offset >= 0.
#define QSQ_ENC_I(WT, IT, RD)                                                  \
  do {                                                                         \
    const WT *s = (const WT *)src;                                             \
    IT *d = (IT *)dst;                                                         \
    double t, r;                                                               \
    for (size_t i = 0; i < nbElts; ++i)                                        \
      d[i] = (IT)QSQ_INDEX(RD);                                                \
  } while (0)

#define qsq_round quant_sqrt_round

// The fold happens in double and before the conversion. Converting a value past
// what the integer holds is undefined and the answer differs by machine, so a
// frame written here would not read back elsewhere. u64 reaches past INT64_MAX on
// real data, hence two routes rather than one through an int64. !(v > lo) so a
// NaN falls to the floor instead of reaching the conversion.
#define QSQ_I64_LO (-9223372036854775808.0)
#define QSQ_I64_HI 9223372036854774784.0  // largest double under INT64_MAX
#define QSQ_U64_HI 18446744073709549568.0 // largest double under UINT64_MAX

static inline int64_t qsq_fit_i(double v) {
  if (!(v > QSQ_I64_LO))
    return INT64_MIN;
  if (!(v < QSQ_I64_HI))
    return (int64_t)QSQ_I64_HI;
  return (int64_t)v;
}

static inline uint64_t qsq_fit_u(double v) {
  if (!(v > 0.0))
    return 0u;
  if (!(v < QSQ_U64_HI))
    return (uint64_t)QSQ_U64_HI;
  return (uint64_t)v;
}

// The value path. The stream carries the reconstruction, rounded to a whole
// number and folded onto the output range, so the decoder only copies or casts.
#define QSQ_ENC_V(WT, OT, RD, FIT)                                             \
  do {                                                                         \
    const WT *s = (const WT *)src;                                             \
    OT *d = (OT *)dst;                                                         \
    double t, r;                                                               \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const int64_t q = QSQ_INDEX(RD);                                         \
      const double w = (double)q * step;                                       \
      double v = w * w - offset;                                               \
      v = v < vlo ? vlo : (v > vhi ? vhi : v);                                 \
      d[i] = (OT)FIT(qsq_round(v));                                            \
    }                                                                          \
  } while (0)

int quant_sqrt_encode(void *restrict dst, const void *restrict src,
                      const quant_sqrt_params *p, int dtype, size_t nbElts) {
  // Same predicate the decoder and the binding read.
  if (!quant_sqrt_params_ok(p, dtype))
    return 1;
  const int values = (p->flags & QUANT_SQRT_FLAG_STORE_VALUES) != 0;

  const double step = p->step, offset = p->offset;
  const double inv = 1.0 / (step * step);
  const double dtop = quant_sqrt_index_top(step, offset, dtype, values);
  const int64_t top = (int64_t)dtop;
  const double vhi = quant_sqrt_value_hi(dtype);
  const double vlo = (p->flags & QUANT_SQRT_FLAG_NONNEGATIVE) != 0
                         ? 0.0
                         : quant_sqrt_value_lo(dtype);

  if (values) {
    switch ((qsq_dtype)dtype) {
    case QSQ_U8:
      QSQ_ENC_V(uint8_t, uint8_t, s[i], qsq_fit_u);
      break;
    case QSQ_U16:
      QSQ_ENC_V(uint16_t, uint16_t, s[i], qsq_fit_u);
      break;
    case QSQ_U32:
      QSQ_ENC_V(uint32_t, uint32_t, s[i], qsq_fit_u);
      break;
    case QSQ_U64:
      QSQ_ENC_V(uint64_t, uint64_t, s[i], qsq_fit_u);
      break;
    case QSQ_I8:
      QSQ_ENC_V(int8_t, int8_t, s[i], qsq_fit_i);
      break;
    case QSQ_I16:
      QSQ_ENC_V(int16_t, int16_t, s[i], qsq_fit_i);
      break;
    case QSQ_I32:
      QSQ_ENC_V(int32_t, int32_t, s[i], qsq_fit_i);
      break;
    case QSQ_I64:
      QSQ_ENC_V(int64_t, int64_t, s[i], qsq_fit_i);
      break;
    case QSQ_F16:
      QSQ_ENC_V(uint16_t, int16_t, quant_sqrt_half_to_float(s[i]), qsq_fit_i);
      break;
    case QSQ_F32:
      QSQ_ENC_V(float, int32_t, s[i], qsq_fit_i);
      break;
    default:
      QSQ_ENC_V(double, int64_t, s[i], qsq_fit_i);
      break;
    }
    return 0;
  }

  // Indices are nonnegative and already checked against the stream width.
  switch ((qsq_dtype)dtype) {
  case QSQ_U8:
    QSQ_ENC_I(uint8_t, uint8_t, s[i]);
    break;
  case QSQ_U16:
    QSQ_ENC_I(uint16_t, uint16_t, s[i]);
    break;
  case QSQ_U32:
    QSQ_ENC_I(uint32_t, uint32_t, s[i]);
    break;
  case QSQ_U64:
    QSQ_ENC_I(uint64_t, uint64_t, s[i]);
    break;
  case QSQ_I8:
    QSQ_ENC_I(int8_t, int8_t, s[i]);
    break;
  case QSQ_I16:
    QSQ_ENC_I(int16_t, int16_t, s[i]);
    break;
  case QSQ_I32:
    QSQ_ENC_I(int32_t, int32_t, s[i]);
    break;
  case QSQ_I64:
    QSQ_ENC_I(int64_t, int64_t, s[i]);
    break;
  case QSQ_F16:
    QSQ_ENC_I(uint16_t, int16_t, quant_sqrt_half_to_float(s[i]));
    break;
  case QSQ_F32:
    QSQ_ENC_I(float, int32_t, s[i]);
    break;
  case QSQ_F64:
    QSQ_ENC_I(double, int64_t, s[i]);
    break;
  }
  return 0;
}

#define QSQ_SCAN(RD)                                                           \
  do {                                                                         \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const double v = (double)(RD);                                           \
      if (!isfinite(v)) {                                                      \
        nf = 1;                                                                \
        continue;                                                              \
      }                                                                        \
      if (v < lo)                                                              \
        lo = v;                                                                \
      if (v > hi)                                                              \
        hi = v;                                                                \
    }                                                                          \
  } while (0)

int quant_sqrt_scan(const void *src, int dtype, size_t nbElts,
                    quant_sqrt_stats *out) {
  double lo = INFINITY, hi = -INFINITY;
  int nf = 0;
  if (src == NULL || out == NULL || !QSQ_DTYPE_OK(dtype))
    return 1;

  switch ((qsq_dtype)dtype) {
  case QSQ_U8:
    QSQ_SCAN(((const uint8_t *)src)[i]);
    break;
  case QSQ_U16:
    QSQ_SCAN(((const uint16_t *)src)[i]);
    break;
  case QSQ_U32:
    QSQ_SCAN(((const uint32_t *)src)[i]);
    break;
  case QSQ_U64:
    QSQ_SCAN(((const uint64_t *)src)[i]);
    break;
  case QSQ_I8:
    QSQ_SCAN(((const int8_t *)src)[i]);
    break;
  case QSQ_I16:
    QSQ_SCAN(((const int16_t *)src)[i]);
    break;
  case QSQ_I32:
    QSQ_SCAN(((const int32_t *)src)[i]);
    break;
  case QSQ_I64:
    QSQ_SCAN(((const int64_t *)src)[i]);
    break;
  case QSQ_F16:
    QSQ_SCAN(quant_sqrt_half_to_float(((const uint16_t *)src)[i]));
    break;
  case QSQ_F32:
    QSQ_SCAN(((const float *)src)[i]);
    break;
  default:
    QSQ_SCAN(((const double *)src)[i]);
    break;
  }

  out->lo = lo;
  out->hi = hi;
  out->anyNegative = lo < 0.0;
  out->anyNonFinite = nf;
  return hi >= lo ? 0 : 1;
}

#undef QSQ_INDEX
#undef QSQ_ENC_I
#undef QSQ_I64_LO
#undef QSQ_I64_HI
#undef QSQ_U64_HI
#undef QSQ_ENC_V
#undef QSQ_SCAN
