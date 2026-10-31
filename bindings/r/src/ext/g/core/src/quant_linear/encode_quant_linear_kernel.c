#include "encode_quant_linear_kernel.h"

#include "quant_linear_check.h"
#include "quant_linear_dtype.h"
#include "quant_linear_half.h"

#include <math.h>
#include <stdint.h>

static double ql_fit(double q, double lo, double hi) {
  if (q != q)
    return 0.0;
  return q < lo ? lo : (q > hi ? hi : q);
}

// Round without forming m + half, which may overflow uint64_t.
static inline uint64_t ql_round_u64(uint64_t m, uint64_t isc, uint64_t half) {
  return m / isc + (m % isc >= isc - half);
}

// Multiply with saturation for legacy values frames.
static inline uint64_t ql_times_u64(uint64_t q, uint64_t isc) {
  return q > UINT64_MAX / isc ? UINT64_MAX : q * isc;
}

// Quantize magnitudes so signed grids remain symmetric around zero.
#define QL_ENC_UV(T)                                                           \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    T *d = (T *)dst;                                                           \
    const uint64_t cap = (uint64_t)(T)(~(T)0);                                 \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const uint64_t q = sizeof(T) < 8 ? ((uint64_t)s[i] + half) / isc         \
                                       : ql_round_u64(s[i], isc, half);        \
      const uint64_t r = values ? ql_times_u64(q, isc) : q;                    \
      d[i] = (T)(r < cap ? r : cap);                                           \
    }                                                                          \
  } while (0)

// LO has magnitude HI + 1.
#define QL_ENC_IV(T, LO, HI)                                                   \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    T *d = (T *)dst;                                                           \
    const uint64_t top = (uint64_t)(HI) + 1u;                                  \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const int neg = s[i] < 0;                                                \
      const uint64_t m = neg ? 0u - (uint64_t)s[i] : (uint64_t)s[i];           \
      const uint64_t q = sizeof(T) < 8 ? (m + half) / isc                      \
                                       : ql_round_u64(m, isc, half);           \
      const uint64_t r = values ? ql_times_u64(q, isc) : q;                    \
      if (neg)                                                                 \
        d[i] = r >= top ? (LO) : (T)(0 - (int64_t)r);                          \
      else                                                                     \
        d[i] = r >= top - 1u ? (HI) : (T)r;                                    \
    }                                                                          \
  } while (0)

// Legacy values frames have an integer step and reconstruction.
#define QL_ENC_F(WT, IT, RD)                                                   \
  do {                                                                         \
    const WT *s = (const WT *)src;                                             \
    IT *d = (IT *)dst;                                                         \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const double q = nearbyint((double)(RD) / step);                          \
      d[i] = (IT)(int64_t)ql_fit(values ? q * step : q, -smax - 1.0, smax);    \
    }                                                                          \
  } while (0)

int quant_linear_encode(void *restrict dst, const void *restrict src,
                        const quant_linear_params *p, int dtype,
                        size_t nbElts) {
  // Same predicate as the decoder.
  if (!quant_linear_params_ok(p, dtype))
    return 1;
  const double step = p->step;
  const double smax = quant_linear_stream_max(dtype);
  const int values = (p->flags & QUANT_LINEAR_FLAG_STORE_VALUES) != 0;
  const uint64_t isc = quant_linear_step_u64(step);
  const uint64_t half = isc >> 1;

  switch ((ql_dtype)dtype) {
  case QL_U8:
    QL_ENC_UV(uint8_t);
    break;
  case QL_U16:
    QL_ENC_UV(uint16_t);
    break;
  case QL_U32:
    QL_ENC_UV(uint32_t);
    break;
  case QL_U64:
    QL_ENC_UV(uint64_t);
    break;
  case QL_I8:
    QL_ENC_IV(int8_t, INT8_MIN, INT8_MAX);
    break;
  case QL_I16:
    QL_ENC_IV(int16_t, INT16_MIN, INT16_MAX);
    break;
  case QL_I32:
    QL_ENC_IV(int32_t, INT32_MIN, INT32_MAX);
    break;
  case QL_I64:
    QL_ENC_IV(int64_t, INT64_MIN, INT64_MAX);
    break;
  case QL_F16:
    QL_ENC_F(uint16_t, int16_t, quant_linear_half_to_float(s[i]));
    break;
  case QL_F32:
    QL_ENC_F(float, int32_t, s[i]);
    break;
  case QL_F64:
    QL_ENC_F(double, int64_t, s[i]);
    break;
  }
  return 0;
}

#define QL_SCAN(RD)                                                            \
  do {                                                                         \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const double v = (double)(RD);                                           \
      if (!isfinite(v))                                                        \
        continue;                                                              \
      haveFinite = 1;                                                          \
      if (v < 0.0)                                                             \
        neg = 1;                                                               \
      const double a = fabs(v);                                                \
      if (a > hi)                                                              \
        hi = a;                                                                \
    }                                                                          \
  } while (0)

int quant_linear_scan(const void *src, int dtype, size_t nbElts,
                      quant_linear_stats *out) {
  double hi = 0.0;
  int neg = 0;
  int haveFinite = 0;
  if (src == NULL || out == NULL || !QL_DTYPE_OK(dtype))
    return 1;

  switch ((ql_dtype)dtype) {
  case QL_U8:
    QL_SCAN(((const uint8_t *)src)[i]);
    break;
  case QL_U16:
    QL_SCAN(((const uint16_t *)src)[i]);
    break;
  case QL_U32:
    QL_SCAN(((const uint32_t *)src)[i]);
    break;
  case QL_U64:
    QL_SCAN(((const uint64_t *)src)[i]);
    break;
  case QL_I8:
    QL_SCAN(((const int8_t *)src)[i]);
    break;
  case QL_I16:
    QL_SCAN(((const int16_t *)src)[i]);
    break;
  case QL_I32:
    QL_SCAN(((const int32_t *)src)[i]);
    break;
  case QL_I64:
    QL_SCAN(((const int64_t *)src)[i]);
    break;
  case QL_F16:
    QL_SCAN(quant_linear_half_to_float(((const uint16_t *)src)[i]));
    break;
  case QL_F32:
    QL_SCAN(((const float *)src)[i]);
    break;
  case QL_F64:
    QL_SCAN(((const double *)src)[i]);
    break;
  }

  out->maxAbs = hi;
  out->anyNegative = neg;
  return haveFinite ? 0 : 1;
}

#undef QL_ENC_UV
#undef QL_ENC_IV
#undef QL_ENC_F
#undef QL_SCAN
