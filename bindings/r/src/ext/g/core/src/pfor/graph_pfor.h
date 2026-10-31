#ifndef GEOZL_CODECS_PFOR_GRAPH_H
#define GEOZL_CODECS_PFOR_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

#define PFOR_GRAPH(id)                                                         \
  {                                                                            \
    .CTid = (id), .inStreamType = ZL_Type_numeric,                             \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_serial)                        \
  }

// Little endian: uint64 element count, uint8 element width.
#define PFOR_HEADER_SIZE (8 + 1)

#endif // GEOZL_CODECS_PFOR_GRAPH_H
