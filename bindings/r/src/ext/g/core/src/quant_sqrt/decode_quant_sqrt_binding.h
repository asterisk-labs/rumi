#ifndef GEOZL_CODECS_QUANT_SQRT_DECODE_BINDING_H
#define GEOZL_CODECS_QUANT_SQRT_DECODE_BINDING_H

#include "graph_quant_sqrt.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_quant_sqrt(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_QUANT_SQRT(id)                                                      \
  {                                                                            \
    .gd = QUANT_SQRT_GRAPH, .transform_f = DI_geozl_quant_sqrt,                \
    .name = "geozl.lossy.quant_sqrt",                                          \
  }

#endif // GEOZL_CODECS_QUANT_SQRT_DECODE_BINDING_H
