// Shared graph definition for the quant_sqrt lossy codec.
// Numeric in, numeric out. The output is always an integer stream.

#ifndef GEOZL_CODECS_QUANT_SQRT_GRAPH_H
#define GEOZL_CODECS_QUANT_SQRT_GRAPH_H

#include "geozl/ctids.h"    // GEOZL_CTID_QUANT_SQRT
#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

#define QUANT_SQRT_CTID GEOZL_CTID_QUANT_SQRT

// dtype is an int param. The parameters hold two doubles, so they ride in a copy
// param rather than an int param that would truncate them. Int and copy ids are
// separate planes, hence the shared number.
#define QUANT_SQRT_PARAM_DTYPE 1
#define QUANT_SQRT_PARAM_PARAMS 1

#define QUANT_SQRT_GRAPH                                                       \
  {                                                                            \
    .CTid = QUANT_SQRT_CTID, .inStreamType = ZL_Type_numeric,                  \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric)                       \
  }

// Wire header, little endian. uint8 dtype, uint8 flags, then step and offset as
// IEEE doubles. The offset is the anchor of the curve and there is no way to fold
// it into the step, so it costs its own eight bytes.
#define QUANT_SQRT_HEADER_SIZE (1 + 1 + 8 + 8)

#endif // GEOZL_CODECS_QUANT_SQRT_GRAPH_H
