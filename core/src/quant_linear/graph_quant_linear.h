// Shared graph definition for the quant_linear lossy codec.
// Numeric in, numeric out. The output is always an integer index stream.

#ifndef RUMI_CODECS_QUANT_LINEAR_GRAPH_H
#define RUMI_CODECS_QUANT_LINEAR_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

// Unique codec id, in the rumi lossy range 0x72D780 to 0x72D7FF
#define QUANT_LINEAR_CTID 0x72D780

// Element type of the original stream, carried in the codec header so the decoder
// can rebuild it. The index stream that travels between encode and decode is always
// a signed integer, the original type only matters at the two ends.
typedef enum {
    QL_U8  = 0,  // unsigned int,  1 byte
    QL_U16 = 1,  // unsigned int,  2 bytes
    QL_U32 = 2,  // unsigned int,  4 bytes
    QL_U64 = 3,  // unsigned int,  8 bytes
    QL_I8  = 4,  // signed int,    1 byte
    QL_I16 = 5,  // signed int,    2 bytes
    QL_I32 = 6,  // signed int,    4 bytes
    QL_I64 = 7,  // signed int,    8 bytes
    QL_F16 = 8,  // IEEE float,    2 bytes
    QL_F32 = 9,  // IEEE float,    4 bytes
    QL_F64 = 10  // IEEE float,    8 bytes
} ql_dtype;

// Local int params the C graph builder sets on the encode node. The scale is a
// double, passed as its 64 bit pattern through an int param. The Python encoder
// does not use these, it sends the codec header directly, these are the C path.
#define QUANT_LINEAR_PARAM_DTYPE      1
#define QUANT_LINEAR_PARAM_SCALE_BITS 2

#define QUANT_LINEAR_GRAPH                                    \
    {                                                         \
        .CTid           = QUANT_LINEAR_CTID,                  \
        .inStreamType   = ZL_Type_numeric,                    \
        .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric)  \
    }

#endif // RUMI_CODECS_QUANT_LINEAR_GRAPH_H
