#include "decode_quant_sqrt_kernel.h"

#include "quant_sqrt_check.h"
#include "quant_sqrt_dtype.h"
#include "quant_sqrt_half.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The rebuild is (q*step)^2 - offset. Everything below exists to keep control
// flow out of the body, since any of it and gcc refuses the loop.
//
// The index is folded first, with one unsigned min rather than a pair of signed
// compares. The encoder never emits a negative index, so one out of a damaged
// frame is nonsense either way and reading it unsigned sends it to the top of
// the grid. A lower bound written as a literal zero turns the expression into a
// branch instead. Checked on gcc 13.3 with -fopt-info-vec.
//
// The clamp is negated so a NaN falls to the floor. Folding the index is not
// enough on its own: a forged step past what a float holds becomes an infinity
// in the narrow paths, and index zero times an infinity is a NaN.

#define QSQ_DEC(WT, IT, UT, ATYPE, STORE)                                      \
  do {                                                                         \
    const IT *s = (const IT *)src;                                             \
    WT *d = (WT *)dst;                                                         \
    const ATYPE st = (ATYPE)step, of = (ATYPE)offset;                          \
    const ATYPE lo = (ATYPE)vlo, hi = (ATYPE)vhi;                              \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      UT u = (UT)s[i];                                                         \
      u = u > utop ? utop : u;                                                 \
      const ATYPE t = (ATYPE)(IT)u * st;                                       \
      ATYPE v = t * t - of;                                                    \
      v = !(v > lo) ? lo : (!(v < hi) ? hi : v);                               \
      d[i] = (WT)(STORE(v));                                                   \
    }                                                                          \
  } while (0)

// The stream already holds the reconstruction, so this is a conversion.
#define QSQ_DEC_V(WT, IT, STORE)                                               \
  do {                                                                         \
    const IT *s = (const IT *)src;                                             \
    WT *d = (WT *)dst;                                                         \
    for (size_t i = 0; i < nbElts; ++i) {                                      \
      IT v = s[i];                                                             \
      v = v < flo ? flo : v;                                                   \
      d[i] = (WT)(STORE((double)v));                                           \
    }                                                                          \
  } while (0)

#define QSQ_ID(v) (v)
#define QSQ_TOF32(v) ((float)(v))
#define QSQ_TOF16(v) (quant_sqrt_float_to_half((float)(v)))

// Rebuild small integer levels once, then decode through the table.
static int qsq_dec_table(void *dst, const void *src, const quant_sqrt_params *p,
                         int dtype, size_t nbElts, size_t w, size_t n) {
  unsigned char *buf = malloc(2 * n * w);
  if (buf == NULL)
    return 1;
  unsigned char *ramp = buf, *tab = buf + n * w;
  for (size_t k = 0; k < n; ++k) {
    if (w == 1)
      ramp[k] = (uint8_t)k;
    else
      ((uint16_t *)ramp)[k] = (uint16_t)k;
  }
  quant_sqrt_decode(tab, ramp, p, dtype, n);
  const size_t top = n - 1;
  if (w == 1) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < nbElts; ++i)
      d[i] = tab[s[i] > top ? top : s[i]];
  } else {
    const uint16_t *s = (const uint16_t *)src;
    const uint16_t *t = (const uint16_t *)tab;
    uint16_t *d = (uint16_t *)dst;
    for (size_t i = 0; i < nbElts; ++i)
      d[i] = t[s[i] > top ? top : s[i]];
  }
  free(buf);
  return 0;
}

int quant_sqrt_decode(void *restrict dst, const void *restrict src,
                      const quant_sqrt_params *p, int dtype, size_t nbElts) {
  // Same predicate the encoder and the binding read.
  if (!quant_sqrt_params_ok(p, dtype))
    return 1;
  const int values = (p->flags & QUANT_SQRT_FLAG_STORE_VALUES) != 0;
  const int nonneg = (p->flags & QUANT_SQRT_FLAG_NONNEGATIVE) != 0;
  const int narrow = (p->flags & QUANT_SQRT_FLAG_DECODE_F32) != 0;

  // Integer VALUES already holds the reconstruction.
  if (dtype <= QSQ_LAST_INT && values) {
    memcpy(dst, src, nbElts * quant_sqrt_width(dtype));
    return 0;
  }

  if (values) {
    switch ((qsq_dtype)dtype) {
    case QSQ_F16: {
      const int16_t flo = nonneg ? 0 : INT16_MIN;
      QSQ_DEC_V(uint16_t, int16_t, QSQ_TOF16);
      break;
    }
    case QSQ_F32: {
      const int32_t flo = nonneg ? 0 : INT32_MIN;
      QSQ_DEC_V(float, int32_t, QSQ_TOF32);
      break;
    }
    default: {
      const int64_t flo = nonneg ? 0 : INT64_MIN;
      QSQ_DEC_V(double, int64_t, QSQ_ID);
      break;
    }
    }
    return 0;
  }

  const double step = p->step, offset = p->offset;
  const double vhi = quant_sqrt_value_hi(dtype);
  const double vlo = nonneg ? 0.0 : quant_sqrt_value_lo(dtype);
  const double dtop = quant_sqrt_index_top(step, offset, dtype, 0);

  if (dtype <= QSQ_LAST_INT && quant_sqrt_width(dtype) <= 2) {
    const size_t w = quant_sqrt_width(dtype);
    const size_t n = (size_t)(w == 1 ? (uint8_t)dtop : (uint16_t)dtop) + 1;
    if (nbElts > 2 * n && qsq_dec_table(dst, src, p, dtype, nbElts, w, n) == 0)
      return 0;
  }

  // Integer indices use the rounding budget reserved by the resolver.
  switch ((qsq_dtype)dtype) {
  case QSQ_U8: {
    const uint8_t utop = (uint8_t)dtop;
    QSQ_DEC(uint8_t, uint8_t, uint8_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_U16: {
    const uint16_t utop = (uint16_t)dtop;
    QSQ_DEC(uint16_t, uint16_t, uint16_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_U32: {
    const uint32_t utop = (uint32_t)dtop;
    QSQ_DEC(uint32_t, uint32_t, uint32_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_U64: {
    const uint64_t utop = (uint64_t)dtop;
    QSQ_DEC(uint64_t, uint64_t, uint64_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_I8: {
    const uint8_t utop = (uint8_t)dtop;
    QSQ_DEC(int8_t, int8_t, uint8_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_I16: {
    const uint16_t utop = (uint16_t)dtop;
    QSQ_DEC(int16_t, int16_t, uint16_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_I32: {
    const uint32_t utop = (uint32_t)dtop;
    QSQ_DEC(int32_t, int32_t, uint32_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_I64: {
    const uint64_t utop = (uint64_t)dtop;
    QSQ_DEC(int64_t, int64_t, uint64_t, double, quant_sqrt_round);
    break;
  }
  case QSQ_F16: {
    const uint16_t utop = (uint16_t)dtop;
    QSQ_DEC(uint16_t, int16_t, uint16_t, float, QSQ_TOF16);
    break;
  }
  case QSQ_F32: {
    const uint32_t utop = (uint32_t)dtop;
    // The bit says which arithmetic the declared bound was measured on.
    if (narrow)
      QSQ_DEC(float, int32_t, uint32_t, float, QSQ_ID);
    else
      QSQ_DEC(float, int32_t, uint32_t, double, QSQ_TOF32);
    break;
  }
  default: {
    const uint64_t utop = (uint64_t)dtop;
    QSQ_DEC(double, int64_t, uint64_t, double, QSQ_ID);
    break;
  }
  }
  return 0;
}

#undef QSQ_DEC
#undef QSQ_DEC_V
#undef QSQ_ID
#undef QSQ_TOF32
#undef QSQ_TOF16
