// Shared graph definition for the delta_w custom codec, used by both the
// encoder and decoder registration. Numeric stream in, numeric stream out.

#ifndef RUMI_CODECS_DELTA_W_GRAPH_H
#define RUMI_CODECS_DELTA_W_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

// Unique codec id, in the rumi experimental range 0x72D7xx
#define DELTAW_CTID 0x72D701

// Local int param carrying the tile row width, set by the rumi graph builder
#define DELTAW_PARAM_WIDTH 1

#define DELTAW_GRAPH                                          \
    {                                                         \
        .CTid           = DELTAW_CTID,                        \
        .inStreamType   = ZL_Type_numeric,                   \
        .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric) \
    }

#endif // RUMI_CODECS_DELTA_W_GRAPH_H
