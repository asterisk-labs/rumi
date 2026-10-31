#ifndef GEOZL_CODECS_DEINTERLEAVE_DECODE_BINDING_H
#define GEOZL_CODECS_DEINTERLEAVE_DECODE_BINDING_H

#include "common/graph_num1to2.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_deinterleave(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_DEINTERLEAVE(id)                                                    \
  {                                                                            \
    .gd = GEOZL_NUM1TO2_GRAPH(id), .transform_f = DI_geozl_deinterleave,       \
    .name = "geozl.lossless.deinterleave",                                     \
  }

#endif // GEOZL_CODECS_DEINTERLEAVE_DECODE_BINDING_H
