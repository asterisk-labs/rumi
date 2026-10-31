#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_BINDING_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_BINDING_H

#include "graph_planar_zigzag_pfor.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_planar_zigzag_pfor(ZL_Encoder *eictx,
                                       const ZL_Input *in);

#define EI_PLANAR_ZIGZAG_PFOR(id)                                              \
  {                                                                            \
    .gd = PLANAR_ZIGZAG_PFOR_GRAPH(id),                                        \
    .transform_f = EI_geozl_planar_zigzag_pfor,                                \
    .name = "geozl.lossless.planar_zigzag_pfor",                               \
  }

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_BINDING_H
