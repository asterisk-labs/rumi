#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_LANES_DECODE_BINDING_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_LANES_DECODE_BINDING_H

#include "graph_planar_zigzag_lanes.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_planar_zigzag_lanes(ZL_Decoder *dictx,
                                       const ZL_Input *ins[]);

#define DI_PLANAR_ZIGZAG_LANES(id)                                             \
  {                                                                            \
    .gd = PLANAR_ZIGZAG_LANES_GRAPH(id),                                       \
    .transform_f = DI_geozl_planar_zigzag_lanes,                               \
    .name = "geozl.lossless.planar_zigzag_lanes",                              \
  }

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_LANES_DECODE_BINDING_H
