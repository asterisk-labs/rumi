#ifndef GEOZL_COMMON_GRAPH_NUM1TO2_H
#define GEOZL_COMMON_GRAPH_NUM1TO2_H

#include "openzl/zl_data.h"

// Shared by every 1->2 numeric codec. ZL_STREAMTYPELIST fills nbOutStreams,
// keep it last.
#define GEOZL_NUM1TO2_GRAPH(id)                                                \
  {                                                                            \
    .CTid = (id), .inStreamType = ZL_Type_numeric,                             \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric, ZL_Type_numeric),     \
  }

#endif // GEOZL_COMMON_GRAPH_NUM1TO2_H
