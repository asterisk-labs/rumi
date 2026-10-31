#include "decode_quant_log_kernel.h"

#include "quant_log_check.h"
#include "quant_log_dtype.h"
#include "quant_log_half.h"
#include "quant_log_math.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Index magnitude, taken so the most negative value does not overflow.
#define QLOG_MAG(q) ((q) < 0 ? (uint64_t)(-((q) + 1)) + 1u : (uint64_t)(q))

// Table budget. Beyond it the long way is taken instead.
#define QLOG_LUT_BYTES 65536

static inline double qlog_clamp(double v, double lo, double hi) {
  if (v != v)
    return 0.0;
  return v < lo ? lo : (v > hi ? hi : v);
}

// The whole of case 3 in one place. The table caches this and the long way calls
// it, so a tile decoded either way comes out bit for bit the same.
static inline double qlog_from_index(int64_t q, double step, int anchor,
                                     double vlo, double vhi) {
  if (q == 0)
    return 0.0;
  const double a = qlog_index_level((double)QLOG_MAG(q), step, anchor);
  return qlog_clamp(q < 0 ? -a : a, vlo, vhi);
}

// The table is signed and covers the index zero, so neither the sign nor the
// zero needs a branch in the loop. Its entries are already at the output width
// and already folded onto the output range, so the loop is a load, a lookup and
// a store. Clamping the index into the tabulated range doubles as the guard
// against a forged stream.
#define QLOG_DEC_INDEX(WT, IT, STORE)                                          \
  do {                                                                         \
    const IT *restrict s = (const IT *)src;                                    \
    WT *restrict d = (WT *)dst;                                                \
    int64_t lo = (int64_t)s[0], hi = lo;                                       \
    for (size_t i = 1; i < nbElts; ++i) {                                      \
      const int64_t v = (int64_t)s[i];                                         \
      lo = v < lo ? v : lo;                                                    \
      hi = v > hi ? v : hi;                                                    \
    }                                                                          \
    const uint64_t span = (uint64_t)hi - (uint64_t)lo + 1u;                    \
    WT *lut = (span != 0 && span <= QLOG_LUT_BYTES / sizeof(WT))               \
                  ? (WT *)malloc((size_t)span * sizeof(WT))                    \
                  : NULL;                                                      \
    if (lut != NULL) {                                                         \
      for (uint64_t k = 0; k < span; ++k)                                      \
        lut[k] = (WT)(STORE(                                                   \
            qlog_from_index(lo + (int64_t)k, step, anchor, vlo, vhi)));         \
      for (size_t i = 0; i < nbElts; ++i) {                                    \
        int64_t v = (int64_t)s[i];                                             \
        v = v < lo ? lo : (v > hi ? hi : v);                                   \
        d[i] = lut[(uint64_t)(v - lo)];                                        \
      }                                                                        \
      free(lut);                                                               \
    } else {                                                                   \
      for (size_t i = 0; i < nbElts; ++i)                                      \
        d[i] = (WT)(STORE(                                                     \
            qlog_from_index((int64_t)s[i], step, anchor, vlo, vhi)));          \
    }                                                                          \
  } while (0)

// Case 2. The stream already holds the reconstruction, so this is a conversion
// and no arithmetic. The floor is a max against a loop invariant, which the
// compiler keeps vectorised.
#define QLOG_DEC_VALUES(WT, IT, STORE)                                         \
  do {                                                                         \
    const IT *restrict s = (const IT *)src;                                    \
    WT *restrict d = (WT *)dst;                                                \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      const IT v = s[i];                                                       \
      d[i] = (WT)(STORE((double)(v < floorv ? floorv : v)));                   \
    }                                                                          \
  } while (0)

#define QLOG_DEC_FLOOR(T)                                                      \
  do {                                                                         \
    const T *restrict s = (const T *)src;                                      \
    T *restrict d = (T *)dst;                                                  \
    for (size_t i = 0; i < nbElts; ++i)                                        \
      d[i] = s[i] < 0 ? (T)0 : s[i];                                           \
  } while (0)

#define QLOG_ID(v) (v)
#define QLOG_F32(v) ((float)(v))
#define QLOG_F16(v) (quant_log_float_to_half((float)(v)))

int quant_log_decode(void *restrict dst, const void *restrict src,
                     const quant_log_params *p, int dtype, size_t nbElts) {
  if (!quant_log_params_ok(p, dtype))
    return 1;
  const int values = (p->flags & QUANT_LOG_FLAG_STORE_VALUES) != 0;
  const int nonneg = (p->flags & QUANT_LOG_FLAG_NONNEGATIVE) != 0;

  // Case 1. The stream is already the output, in the output type. An unsigned
  // one carries the floor already; without applying it to a signed one the flag
  // would mean the floor on a float frame and nothing on an integer one.
  if (dtype <= QLOG_LAST_INT) {
    if (nonneg && dtype >= QLOG_I8) {
      switch ((qlog_dtype)dtype) {
      case QLOG_I8:
        QLOG_DEC_FLOOR(int8_t);
        break;
      case QLOG_I16:
        QLOG_DEC_FLOOR(int16_t);
        break;
      case QLOG_I32:
        QLOG_DEC_FLOOR(int32_t);
        break;
      default:
        QLOG_DEC_FLOOR(int64_t);
        break;
      }
      return 0;
    }
    memcpy(dst, src, nbElts * quant_log_width(dtype));
    return 0;
  }
  if (nbElts == 0)
    return 0;

  if (values) {
    switch ((qlog_dtype)dtype) {
    case QLOG_F16: {
      const int16_t floorv = nonneg ? 0 : INT16_MIN;
      QLOG_DEC_VALUES(uint16_t, int16_t, QLOG_F16);
      break;
    }
    case QLOG_F32: {
      const int32_t floorv = nonneg ? 0 : INT32_MIN;
      QLOG_DEC_VALUES(float, int32_t, QLOG_F32);
      break;
    }
    default: {
      const int64_t floorv = nonneg ? 0 : INT64_MIN;
      QLOG_DEC_VALUES(double, int64_t, QLOG_ID);
      break;
    }
    }
    return 0;
  }

  const double step = p->step;
  const int anchor = quant_log_anchor_exp2(dtype);
  const double vhi = quant_log_value_hi(dtype);
  const double vlo = nonneg ? 0.0 : quant_log_value_lo(dtype);

  switch ((qlog_dtype)dtype) {
  case QLOG_F16:
    QLOG_DEC_INDEX(uint16_t, int16_t, QLOG_F16);
    break;
  case QLOG_F32:
    QLOG_DEC_INDEX(float, int32_t, QLOG_F32);
    break;
  default:
    QLOG_DEC_INDEX(double, int64_t, QLOG_ID);
    break;
  }
  return 0;
}

#undef QLOG_MAG
#undef QLOG_LUT_BYTES
#undef QLOG_DEC_INDEX
#undef QLOG_DEC_VALUES
#undef QLOG_DEC_FLOOR
#undef QLOG_ID
#undef QLOG_F32
#undef QLOG_F16
