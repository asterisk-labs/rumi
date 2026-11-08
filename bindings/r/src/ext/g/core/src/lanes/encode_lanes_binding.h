#ifndef GEOZL_CODECS_LANES_ENCODE_BINDING_H
#define GEOZL_CODECS_LANES_ENCODE_BINDING_H

#include "graph_lanes.h"
#include "openzl/zl_ctransform.h"

ZL_Report EI_geozl_lanes(ZL_Encoder *encoder, const ZL_Input *input);

#define EI_LANES(id)                                                           \
  {                                                                            \
    .gd = LANES_GRAPH(id), .transform_f = EI_geozl_lanes,                      \
    .name = "geozl.lossless.lanes",                                            \
  }

#endif // GEOZL_CODECS_LANES_ENCODE_BINDING_H
