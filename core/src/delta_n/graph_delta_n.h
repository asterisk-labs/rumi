// Shared graph definition for the delta_n custom codec. Numeric in, numeric out.

#ifndef RUMI_CODECS_DELTA_N_GRAPH_H
#define RUMI_CODECS_DELTA_N_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

// Unique codec id, in the rumi experimental range 0x72D7xx
#define DELTAN_CTID 0x72D702

// Local int param carrying the tile row width, set by the rumi graph builder
#define DELTAN_PARAM_WIDTH 1

#define DELTAN_GRAPH                                          \
    {                                                         \
        .CTid           = DELTAN_CTID,                        \
        .inStreamType   = ZL_Type_numeric,                   \
        .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric) \
    }

#endif // RUMI_CODECS_DELTA_N_GRAPH_H
