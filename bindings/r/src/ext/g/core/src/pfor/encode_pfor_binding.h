#ifndef GEOZL_CODECS_PFOR_ENCODE_BINDING_H
#define GEOZL_CODECS_PFOR_ENCODE_BINDING_H

#include "graph_pfor.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_pfor(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_PFOR(id)                                                            \
  {                                                                            \
    .gd = PFOR_GRAPH(id), .transform_f = EI_geozl_pfor,                        \
    .name = "geozl.lossless.pfor",                                             \
  }

#endif // GEOZL_CODECS_PFOR_ENCODE_BINDING_H
