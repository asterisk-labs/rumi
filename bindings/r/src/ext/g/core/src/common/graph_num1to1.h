#ifndef GEOZL_COMMON_GRAPH_NUM1TO1_H
#define GEOZL_COMMON_GRAPH_NUM1TO1_H

#include "openzl/zl_data.h"

// Shared by every predictor. ZL_STREAMTYPELIST fills nbOutStreams, keep it
// last.
#define GEOZL_NUM1TO1_GRAPH(id)                                                \
  {                                                                            \
    .CTid = (id), .inStreamType = ZL_Type_numeric,                             \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric),                      \
  }

#define GEOZL_PARAM_WIDTH 1
// Stacked planes. A predictor that reads the row above restarts at every plane
// boundary, since those two rows sit a whole image apart.
#define GEOZL_PARAM_PLANES 2

#endif // GEOZL_COMMON_GRAPH_NUM1TO1_H