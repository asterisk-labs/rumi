#ifndef GEOZL_CODECS_DELTA_W_DECODE_BINDING_H
#define GEOZL_CODECS_DELTA_W_DECODE_BINDING_H

#include "common/graph_num1to1.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_delta_w(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_DELTA_W(id)                                                         \
  {                                                                            \
    .gd = GEOZL_NUM1TO1_GRAPH(id), .transform_f = DI_geozl_delta_w,            \
    .name = "geozl.lossless.delta_w",                                          \
  }

#endif // GEOZL_CODECS_DELTA_W_DECODE_BINDING_H
