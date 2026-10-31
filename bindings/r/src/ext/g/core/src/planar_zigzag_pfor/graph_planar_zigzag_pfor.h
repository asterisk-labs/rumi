#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_GRAPH_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

#define PLANAR_ZIGZAG_PFOR_GRAPH(id)                                          \
  {                                                                            \
    .CTid = (id), .inStreamType = ZL_Type_numeric,                             \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_serial)                        \
  }

// Little endian: uint64 element count, uint8 element width, uint32 row width,
// uint32 plane count.
#define PLANAR_ZIGZAG_PFOR_HEADER_SIZE (8 + 1 + 4 + 4)

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_GRAPH_H
