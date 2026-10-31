#ifndef GEOZL_CODECS_QUANT_LOG_DTYPE_H
#define GEOZL_CODECS_QUANT_LOG_DTYPE_H

#include "common/fp.h"

#include "geozl/quant_log_params.h"

#include <stddef.h>
#include <stdint.h>

#define QLOG_FINITE_TOP GEOZL_F64_MAX

// Wire codes, frozen.
typedef enum {
  QLOG_U8 = 0,
  QLOG_U16 = 1,
  QLOG_U32 = 2,
  QLOG_U64 = 3,
  QLOG_I8 = 4,
  QLOG_I16 = 5,
  QLOG_I32 = 6,
  QLOG_I64 = 7,
  QLOG_F16 = 8,
  QLOG_F32 = 9,
  QLOG_F64 = 10
} qlog_dtype;

#define QLOG_LAST_INT QLOG_I64
#define QLOG_DTYPE_OK(d) ((d) >= QLOG_U8 && (d) <= QLOG_F64)

// Codes have no gaps, so a code indexes this.
static inline size_t quant_log_width(int dtype) {
  static const size_t w[] = {1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8};
  return w[dtype];
}

// Exponent of the smallest positive value the type holds. The index grid anchors
// here, which keeps every index magnitude at or above one and leaves zero to
// mean zero.
static inline int quant_log_anchor_exp2(int dtype) {
  switch (dtype) {
  case QLOG_F16:
    return -24;
  case QLOG_F32:
    return -149;
  default:
    return -1074;
  }
}

// log2 of the largest magnitude over the anchor, so the whole width of the index
// grid. Whole numbers because both ends are powers of two up to the mantissa.
static inline double quant_log_index_span(int dtype) {
  switch (dtype) {
  case QLOG_F16:
    return 40.0;
  case QLOG_F32:
    return 277.0;
  default:
    return 2098.0;
  }
}

// log2 of the largest reconstruction the value grid may produce, which anchors
// at one. On a float it is capped by the largest whole number the type carries
// with no gap, since past that a cast back would round.
static inline double quant_log_value_span(int dtype) {
  switch (dtype) {
  case QLOG_U8:
  case QLOG_I8:
    return 8.0;
  case QLOG_U16:
  case QLOG_I16:
    return 16.0;
  case QLOG_U32:
  case QLOG_I32:
    return 32.0;
  case QLOG_U64:
  case QLOG_I64:
    return 64.0;
  case QLOG_F16:
    return 11.0;
  case QLOG_F32:
    return 24.0;
  default:
    return 53.0;
  }
}

// Half an ulp of a normal value. A subnormal k*2^E has neighbours a flat 2^E
// apart, so its relative rounding is 0.5/k and no constant covers it; the bound
// is stated from the smallest normal up.
static inline double quant_log_eps(int dtype) {
  switch (dtype) {
  case QLOG_F16:
    return 4.8828125e-4; // 2^-11
  case QLOG_F32:
    return 5.9604644775390625e-8; // 2^-24
  case QLOG_F64:
    return 1.1102230246251565e-16; // 2^-53
  default:
    return 0.0;
  }
}

static inline double quant_log_normal_min(int dtype) {
  switch (dtype) {
  case QLOG_F16:
    return 6.103515625e-5; // 2^-14
  case QLOG_F32:
    return 1.17549435082228751e-38; // 2^-126
  default:
    return 2.22507385850720138e-308; // 2^-1022
  }
}

// Largest whole number the type carries with no gap. A values frame cannot reach
// past it, since the reconstruction would round on the way back.
static inline double quant_log_exact_int(int dtype) {
  switch (dtype) {
  case QLOG_F16:
    return 2048.0; // 2^11
  case QLOG_F32:
    return 16777216.0; // 2^24
  default:
    return GEOZL_F64_EXACT_INT;
  }
}

// Largest index magnitude the stream carries. Only a float takes the index
// path, and its stream is signed at the width of the original type.
static inline double quant_log_stream_max(int dtype) {
  switch (dtype) {
  case QLOG_F16:
    return 32767.0;
  case QLOG_F32:
    return 2147483647.0;
  default:
    return GEOZL_F64_EXACT_INT; // past it an index is not exact in a double
  }
}

static inline double quant_log_value_hi(int dtype) {
  switch (dtype) {
  case QLOG_U8:
    return 255.0;
  case QLOG_U16:
    return 65535.0;
  case QLOG_U32:
    return 4294967295.0;
  case QLOG_U64:
    return 18446744073709549568.0;
  case QLOG_I8:
    return 127.0;
  case QLOG_I16:
    return 32767.0;
  case QLOG_I32:
    return 2147483647.0;
  case QLOG_I64:
    return 9223372036854774784.0;
  case QLOG_F16:
    return 65504.0;
  case QLOG_F32:
    return 3.40282346638528860e38;
  default:
    return 1.79769313486231571e308;
  }
}

// The largest magnitude a type holds, which the value grid folds onto. A signed
// type reaches one step further below zero than above it, and folding onto the
// maximum brings its most negative value back one short.
static inline double quant_log_value_mag(int dtype) {
  switch (dtype) {
  case QLOG_I8:
    return 128.0;
  case QLOG_I16:
    return 32768.0;
  case QLOG_I32:
    return 2147483648.0;
  case QLOG_I64:
    return 9223372036854775808.0;
  default:
    return quant_log_value_hi(dtype);
  }
}

// The floor the decoder clamps against on a tile that held a negative sample.
// Only a float takes the index path, so no integer type asks.
static inline double quant_log_value_lo(int dtype) {
  switch (dtype) {
  case QLOG_F16:
    return -65504.0;
  case QLOG_F32:
    return -3.40282346638528860e38;
  default:
    return -1.79769313486231571e308;
  }
}

// Both transforms work in units of log2 over the anchor, which a double carries
// with a relative error of 2^-53. The series in quant_log_math.h add less, so
// eight units bound either one. Charged twice when the step is cut, once for the
// index the encoder picks and once for the level rebuilt from it.
static inline double quant_log_slack(int dtype, int storeValues) {
  const double span = storeValues ? quant_log_value_span(dtype)
                                  : quant_log_index_span(dtype);
  return 8.0 * 1.1102230246251565e-16 * span;
}

// Largest index magnitude the grid produces. Both kernels fold onto it, so a
// forged step cannot walk the reconstruction past the type. The ceiling is taken
// by hand and guarded above, which keeps the kernels free of libm.
static inline double quant_log_index_top(double step, int dtype) {
  const double lim = quant_log_stream_max(dtype);
  const double v = quant_log_index_span(dtype) / step;
  if (!(v < lim)) // also catches nan
    return lim;
  const double t = (double)(int64_t)v;
  return 1.0 + (t < v ? t + 1.0 : t);
}

// Highest index the value grid reaches. Folded in double first, since a step
// small enough sends the quotient past what an int64 holds.
static inline double quant_log_value_top(double step, int dtype) {
  const double v = quant_log_value_span(dtype) / step;
  if (!(v < GEOZL_F64_EXACT_INT)) // also catches nan
    return GEOZL_F64_EXACT_INT;
  return (double)(int64_t)v + 1.0;
}

#endif // GEOZL_CODECS_QUANT_LOG_DTYPE_H
