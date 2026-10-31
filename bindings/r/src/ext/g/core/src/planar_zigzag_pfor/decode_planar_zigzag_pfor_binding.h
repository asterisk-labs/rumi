#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_DECODE_BINDING_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_DECODE_BINDING_H

#include "graph_planar_zigzag_pfor.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_planar_zigzag_pfor(ZL_Decoder *dictx,
                                       const ZL_Input *ins[]);

#define DI_PLANAR_ZIGZAG_PFOR(id)                                              \
  {                                                                            \
    .gd = PLANAR_ZIGZAG_PFOR_GRAPH(id),                                        \
    .transform_f = DI_geozl_planar_zigzag_pfor,                                \
    .name = "geozl.lossless.planar_zigzag_pfor",                               \
  }

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_DECODE_BINDING_H
