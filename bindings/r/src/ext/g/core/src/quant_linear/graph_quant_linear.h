// Shared graph definition for the quant_linear lossy codec.
// Numeric in, numeric out. The output is always an integer stream.

#ifndef GEOZL_CODECS_QUANT_LINEAR_GRAPH_H
#define GEOZL_CODECS_QUANT_LINEAR_GRAPH_H

#include "geozl/ctids.h"    // GEOZL_CTID_QUANT_LINEAR
#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

#define QUANT_LINEAR_CTID GEOZL_CTID_QUANT_LINEAR

// dtype is an int param. The parameters hold a double, so they ride in a copy
// param rather than an int param that would truncate it. Int and copy ids are
// separate planes, hence the shared number.
#define QUANT_LINEAR_PARAM_DTYPE 1
#define QUANT_LINEAR_PARAM_PARAMS 1

#define QUANT_LINEAR_GRAPH                                                     \
  {                                                                            \
    .CTid = QUANT_LINEAR_CTID, .inStreamType = ZL_Type_numeric,                \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric)                       \
  }

// Wire header, little endian. uint8 dtype, uint8 flags, then step as an IEEE
// double.
#define QUANT_LINEAR_HEADER_SIZE (1 + 1 + 8)

#endif // GEOZL_CODECS_QUANT_LINEAR_GRAPH_H
