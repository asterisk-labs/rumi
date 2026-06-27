// Shared graph definition for the delta_n custom codec. Numeric in, numeric out.

#ifndef RUMI_CODECS_DELTA_N_GRAPH_H
#define RUMI_CODECS_DELTA_N_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

// Unique codec id, in the rumi experimental range 0x72D7xx
#define DELTA_N_CTID 0x72D702

// Local int param carrying the tile row width, set by the rumi graph builder
#define DELTA_N_PARAM_WIDTH 1

#define DELTA_N_GRAPH                                         \
    {                                                         \
        .CTid           = DELTA_N_CTID,                       \
        .inStreamType   = ZL_Type_numeric,                   \
        .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric) \
    }

#endif // RUMI_CODECS_DELTA_N_GRAPH_H
