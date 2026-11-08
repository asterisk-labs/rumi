#include "decode_quant_linear_kernel.h"

#include "quant_linear_check.h"
#include "quant_linear_dtype.h"
#include "quant_linear_half.h"

#include <stdint.h>

// Two selects and no branch, so the index loops below vectorise.
static double ql_clamp(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Clamping into an interval that holds x can only shorten the error.
static double ql_lo(const quant_linear_params *p, int dtype) {
  return (p->flags & QUANT_LINEAR_FLAG_NONNEGATIVE) != 0
             ? 0.0
             : quant_linear_value_lo(dtype);
}

// WT is twice as wide as T. Cap the step so the product cannot overflow WT.
#define QL_DEC_IDX_U(T, WT)                                                    \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    T *d = (T *)dst;                                                           \
    const WT cap = (WT)(T)(~(T)0);                                             \
    const WT k = isc > cap ? cap : (WT)isc;                                    \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const WT v = (WT)s[i] * k;                                               \
      d[i] = (T)(v > cap ? cap : v);                                           \
    }                                                                          \
  } while (0)

// Signed saturation uses HI + 1, the magnitude of the type's minimum.
#define QL_DEC_IDX_I(T, WT, UT, HI)                                            \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    T *d = (T *)dst;                                                           \
    const UT top = (UT)(HI) + 1u;                                              \
    const UT k = isc > top ? top : (UT)isc;                                    \
    const WT lo = floorZero ? 0 : -(WT)top;                                    \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const WT q = s[i];                                                       \
      const UT m = (UT)(q < 0 ? -q : q) * k;                                   \
      const WT mr = (WT)(m > top ? top : m);                                   \
      const WT r = q < 0 ? -mr : mr;                                           \
      d[i] = (T)(r < lo ? lo : (r > (WT)(HI) ? (WT)(HI) : r));                 \
    }                                                                          \
  } while (0)

// Check before multiplying because there is no wider integer type.
static void ql_dec_u64(uint64_t *d, const uint64_t *s, uint64_t isc,
                       size_t nbElts) {
  const uint64_t lim = UINT64_MAX / isc;
  for (size_t i = 0; i < nbElts; ++i)
    d[i] = s[i] > lim ? UINT64_MAX : s[i] * isc;
}

static void ql_dec_i64(int64_t *d, const int64_t *s, uint64_t isc,
                       int floorZero, size_t nbElts) {
  const uint64_t top = (uint64_t)INT64_MAX + 1u;
  const uint64_t lim = top / isc;
  for (size_t i = 0; i < nbElts; ++i) {
    const int neg = s[i] < 0;
    const uint64_t m = neg ? 0u - (uint64_t)s[i] : (uint64_t)s[i];
    const uint64_t mr = m > lim ? top : m * isc;
    if (neg)
      d[i] = floorZero ? 0 : (mr >= top ? INT64_MIN : -(int64_t)mr);
    else
      d[i] = mr >= top ? INT64_MAX : (int64_t)mr;
  }
}

#define QL_DEC_MUL(WT, IT, CAST)                                               \
  do {                                                                         \
    const IT *s = (const IT *)src;                                             \
    WT *d = (WT *)dst;                                                         \
    for (size_t i = 0; i < nbElts; ++i)                                        \
      d[i] = (WT)CAST(ql_clamp((double)s[i] * step, vlo, vhi));                \
  } while (0)

int quant_linear_decode(void *restrict dst, const void *restrict src,
                        const quant_linear_params *p, int dtype,
                        size_t nbElts) {
  // Same predicate the encoder reads. The clamp leans on a finite step.
  if (!quant_linear_params_ok(p, dtype))
    return 1;

  const double step = p->step;
  const double vlo = ql_lo(p, dtype), vhi = quant_linear_value_hi(dtype);
  const int floorZero = (p->flags & QUANT_LINEAR_FLAG_NONNEGATIVE) != 0;

  if (dtype <= QL_LAST_INT) {
    // Rebuild integer indices with the encoder's whole step.
    const uint64_t isc = quant_linear_step_u64(step);
    switch ((ql_dtype)dtype) {
    case QL_U8:
      QL_DEC_IDX_U(uint8_t, uint32_t);
      break;
    case QL_U16:
      QL_DEC_IDX_U(uint16_t, uint32_t);
      break;
    case QL_U32:
      QL_DEC_IDX_U(uint32_t, uint64_t);
      break;
    case QL_U64:
      ql_dec_u64((uint64_t *)dst, (const uint64_t *)src, isc, nbElts);
      break;
    case QL_I8:
      QL_DEC_IDX_I(int8_t, int32_t, uint32_t, INT8_MAX);
      break;
    case QL_I16:
      QL_DEC_IDX_I(int16_t, int32_t, uint32_t, INT16_MAX);
      break;
    case QL_I32:
      QL_DEC_IDX_I(int32_t, int64_t, uint64_t, INT32_MAX);
      break;
    default:
      ql_dec_i64((int64_t *)dst, (const int64_t *)src, isc, floorZero,
                 nbElts);
      break;
    }
    return 0;
  }

  switch ((ql_dtype)dtype) {
  case QL_F16: {
    const int16_t *s = (const int16_t *)src;
    uint16_t *d = (uint16_t *)dst;
    for (size_t i = 0; i < nbElts; ++i)
      d[i] = ql_f32_to_half((float)ql_clamp((double)s[i] * step, vlo, vhi));
    break;
  }
  case QL_F32:
    QL_DEC_MUL(float, int32_t, (float));
    break;
  default:
    QL_DEC_MUL(double, int64_t, );
    break;
  }
  return 0;
}

#undef QL_DEC_MUL
