#ifndef GEOZL_CODECS_LANES_GRAPH_H
#define GEOZL_CODECS_LANES_GRAPH_H

#include "pivco_lanes/pivco_lanes.h"

#include "openzl/zl_data.h"

#define LANES_GRAPH(id)                                                        \
  {                                                                            \
    .CTid = (id), .inStreamType = ZL_Type_numeric,                             \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric, ZL_Type_serial)       \
  }

// Little endian: uint64 element count, uint8 element width, uint32 PivCo block
// size, then one lane record per byte in an element.
#define LANES_HEADER_FIXED (8 + 1 + 4)
#define LANES_HEADER_MAX (LANES_HEADER_FIXED + 8 * GEOZL_PIVCO_LANE_HEADER_SIZE)

#endif // GEOZL_CODECS_LANES_GRAPH_H
