#ifndef GEOZL_CODECS_QUANT_SQRT_DTYPE_H
#define GEOZL_CODECS_QUANT_SQRT_DTYPE_H

#include "common/fp.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

// Wire codes, frozen. A published code never changes meaning.
typedef enum {
  QSQ_U8 = 0,
  QSQ_U16 = 1,
  QSQ_U32 = 2,
  QSQ_U64 = 3,
  QSQ_I8 = 4,
  QSQ_I16 = 5,
  QSQ_I32 = 6,
  QSQ_I64 = 7,
  QSQ_F16 = 8,
  QSQ_F32 = 9,
  QSQ_F64 = 10
} qsq_dtype;

#define QSQ_LAST_INT QSQ_I64
#define QSQ_DTYPE_OK(d) ((d) >= QSQ_U8 && (d) <= QSQ_F64)

// Codes have no gaps, so a code indexes this. Callers check the range first.
static inline size_t quant_sqrt_width(int dtype) {
  static const size_t w[] = {1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8};
  return w[dtype];
}

// Half an ulp of a normal value, the relative rounding a stored reconstruction
// suffers. Zero on an integer, where the rounding is half a unit instead.
static inline double quant_sqrt_eps(int dtype) {
  switch (dtype) {
  case QSQ_F16:
    return 4.8828125e-4; // 2^-11
  case QSQ_F32:
    return 5.9604644775390625e-8; // 2^-24
  case QSQ_F64:
    return 1.1102230246251565e-16; // 2^-53
  default:
    return 0.0;
  }
}

// Largest magnitude the stream carries. It keeps the element width of the
// original type, signed once the original was a float.
static inline double quant_sqrt_stream_max(int dtype) {
  switch (dtype) {
  case QSQ_U8:
    return 255.0;
  case QSQ_U16:
    return 65535.0;
  case QSQ_U32:
    return 4294967295.0;
  case QSQ_I8:
    return 127.0;
  case QSQ_I16:
  case QSQ_F16:
    return 32767.0;
  case QSQ_I32:
  case QSQ_F32:
    return 2147483647.0;
  default: // past 2^53 an index is not exact in the double that computes it
    return GEOZL_F64_EXACT_INT;
  }
}

static inline double quant_sqrt_value_hi(int dtype) {
  switch (dtype) {
  case QSQ_U8:
    return 255.0;
  case QSQ_U16:
    return 65535.0;
  case QSQ_U32:
    return 4294967295.0;
  case QSQ_U64:
    return 18446744073709549568.0;
  case QSQ_I8:
    return 127.0;
  case QSQ_I16:
    return 32767.0;
  case QSQ_I32:
    return 2147483647.0;
  case QSQ_I64:
    return 9223372036854774784.0;
  case QSQ_F16:
    return 65504.0;
  case QSQ_F32:
    return 3.40282346638528860e38;
  default:
    return 1.79769313486231571e308;
  }
}

static inline double quant_sqrt_value_lo(int dtype) {
  switch (dtype) {
  case QSQ_I8:
    return -128.0;
  case QSQ_I16:
    return -32768.0;
  case QSQ_I32:
    return -2147483648.0;
  case QSQ_I64:
    return -9223372036854775808.0;
  case QSQ_F16:
    return -65504.0;
  case QSQ_F32:
    return -3.40282346638528860e38;
  case QSQ_F64:
    return -1.79769313486231571e308;
  default:
    return 0.0;
  }
}

// Largest index the grid produces. Both kernels fold onto it once per call, so a
// forged step cannot walk a reconstruction past the type.
//
// The ceiling depends on where the index goes. On the index path it is written
// into the stream and has to fit an element. On the value path it never leaves
// the encoder and only has to stay exact in the double that carries it. Reading
// stream_max there capped a u8 grid at 255 levels.
//
// The sqrt runs once per stream. IEEE has it correctly rounded, so the ceiling is
// the same number on every machine that reads the frame.
static inline double quant_sqrt_index_top(double step, double offset, int dtype,
                                          int values) {
  const double lim =
      values ? GEOZL_F64_EXACT_INT : quant_sqrt_stream_max(dtype);
  if (!(step > 0.0) || !(offset >= 0.0))
    return 1.0;
  const double hi = quant_sqrt_value_hi(dtype) + offset;
  if (!(hi > 0.0))
    return 1.0;
  const double v = sqrt(hi) / step;
  if (!(v < lim)) // also catches nan
    return lim;
  const double t = (double)(int64_t)v;
  const double c = 1.0 + (t < v ? t + 1.0 : t);
  return c > lim ? lim : c;
}

// Integer INDEX encode and decode share half-away-from-zero rounding.
static inline double quant_sqrt_round(double v) {
  return v < 0.0 ? -floor(0.5 - v) : floor(v + 0.5);
}

#endif // GEOZL_CODECS_QUANT_SQRT_DTYPE_H
