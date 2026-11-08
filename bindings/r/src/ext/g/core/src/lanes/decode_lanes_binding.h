#ifndef GEOZL_CODECS_LANES_DECODE_BINDING_H
#define GEOZL_CODECS_LANES_DECODE_BINDING_H

#include "graph_lanes.h"
#include "openzl/zl_dtransform.h"

ZL_Report DI_geozl_lanes(ZL_Decoder *decoder,
                         const ZL_Input *inputs[]);

#define DI_LANES(id)                                                           \
  {                                                                            \
    .gd = LANES_GRAPH(id), .transform_f = DI_geozl_lanes,                      \
    .name = "geozl.lossless.lanes",                                            \
  }

#endif // GEOZL_CODECS_LANES_DECODE_BINDING_H
