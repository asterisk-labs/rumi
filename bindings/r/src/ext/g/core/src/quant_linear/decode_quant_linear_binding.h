#ifndef GEOZL_CODECS_QUANT_LINEAR_DECODE_BINDING_H
#define GEOZL_CODECS_QUANT_LINEAR_DECODE_BINDING_H

#include "graph_quant_linear.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_quant_linear(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_QUANT_LINEAR(id)                                                    \
  {                                                                            \
    .gd = QUANT_LINEAR_GRAPH, .transform_f = DI_geozl_quant_linear,            \
    .name = "geozl.lossy.quant_linear",                                        \
  }

#endif // GEOZL_CODECS_QUANT_LINEAR_DECODE_BINDING_H
