#ifndef GEOZL_CODECS_PFOR_DECODE_BINDING_H
#define GEOZL_CODECS_PFOR_DECODE_BINDING_H

#include "graph_pfor.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_pfor(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_PFOR(id)                                                            \
  {                                                                            \
    .gd = PFOR_GRAPH(id), .transform_f = DI_geozl_pfor,                        \
    .name = "geozl.lossless.pfor",                                             \
  }

#endif // GEOZL_CODECS_PFOR_DECODE_BINDING_H
