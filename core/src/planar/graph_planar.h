// Shared graph definition for the planar custom codec. Numeric in, numeric out.

#ifndef RUMI_CODECS_PLANAR_GRAPH_H
#define RUMI_CODECS_PLANAR_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

// Unique codec id, in the rumi experimental range 0x72D7xx
#define PLANAR_CTID 0x72D703

// Local int param carrying the tile row width, set by the rumi graph builder
#define PLANAR_PARAM_WIDTH 1

#define PLANAR_GRAPH                                          \
    {                                                         \
        .CTid           = PLANAR_CTID,                        \
        .inStreamType   = ZL_Type_numeric,                   \
        .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric) \
    }

#endif // RUMI_CODECS_PLANAR_GRAPH_H
