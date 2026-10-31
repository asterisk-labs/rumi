#ifndef GEOZL_CODECS_QUANT_LINEAR_DTYPE_H
#define GEOZL_CODECS_QUANT_LINEAR_DTYPE_H

// Wire codes, frozen. Carried in the codec header so the decoder rebuilds the
// original type.

#include "common/fp.h"

#include <stddef.h>

typedef enum {
  QL_U8 = 0,
  QL_U16 = 1,
  QL_U32 = 2,
  QL_U64 = 3,
  QL_I8 = 4,
  QL_I16 = 5,
  QL_I32 = 6,
  QL_I64 = 7,
  QL_F16 = 8,
  QL_F32 = 9,
  QL_F64 = 10
} ql_dtype;

#define QL_LAST_INT QL_I64
#define QL_DTYPE_OK(d) ((d) >= QL_U8 && (d) <= QL_F64)

// Codes have no gaps, so a code indexes this. Callers check the range first.
static inline size_t quant_linear_width(int dtype) {
  static const size_t w[] = {1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8};
  return w[dtype];
}

// Relative rounding a stored reconstruction suffers. Zero on an integer.
static inline double quant_linear_eps(int dtype) {
  switch (dtype) {
  case QL_F16:
    return 4.8828125e-4; // 2^-11
  case QL_F32:
    return 5.9604644775390625e-8; // 2^-24
  case QL_F64:
    return 1.1102230246251565e-16; // 2^-53
  default:
    return 0.0;
  }
}

// Largest magnitude the stream carries. It keeps the original element width,
// signed once the original was a float.
static inline double quant_linear_stream_max(int dtype) {
  switch (dtype) {
  case QL_U8:
    return 255.0;
  case QL_U16:
    return 65535.0;
  case QL_U32:
    return 4294967295.0;
  case QL_I8:
    return 127.0;
  case QL_I16:
  case QL_F16:
    return 32767.0;
  case QL_I32:
  case QL_F32:
    return 2147483647.0;
  default: // past 2^53 an index is not exact in the double that computes it
    return GEOZL_F64_EXACT_INT;
  }
}

// Both ends read the output range from here, so a reconstruction cannot be in
// range on one side and wrapped on the other.
static inline double quant_linear_value_lo(int dtype) {
  switch (dtype) {
  case QL_I8:
    return -128.0;
  case QL_I16:
    return -32768.0;
  case QL_I32:
    return -2147483648.0;
  case QL_I64:
    return -9223372036854775808.0;
  case QL_F16:
    return -65504.0;
  case QL_F32:
    return -3.40282346638528860e38;
  case QL_F64:
    return -1.79769313486231571e308;
  default:
    return 0.0;
  }
}

static inline double quant_linear_value_hi(int dtype) {
  switch (dtype) {
  case QL_U8:
    return 255.0;
  case QL_U16:
    return 65535.0;
  case QL_U32:
    return 4294967295.0;
  case QL_U64:
    return 18446744073709549568.0;
  case QL_I8:
    return 127.0;
  case QL_I16:
    return 32767.0;
  case QL_I32:
    return 2147483647.0;
  case QL_I64:
    return 9223372036854774784.0;
  case QL_F16:
    return 65504.0;
  case QL_F32:
    return 3.40282346638528860e38;
  default:
    return 1.79769313486231571e308;
  }
}

#endif // GEOZL_CODECS_QUANT_LINEAR_DTYPE_H
