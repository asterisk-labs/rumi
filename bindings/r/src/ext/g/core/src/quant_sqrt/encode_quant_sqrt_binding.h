#ifndef GEOZL_CODECS_QUANT_SQRT_ENCODE_BINDING_H
#define GEOZL_CODECS_QUANT_SQRT_ENCODE_BINDING_H

#include "graph_quant_sqrt.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_quant_sqrt(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_QUANT_SQRT(id)                                                      \
  {                                                                            \
    .gd = QUANT_SQRT_GRAPH, .transform_f = EI_geozl_quant_sqrt,                \
    .name = "geozl.lossy.quant_sqrt",                                          \
  }

#endif // GEOZL_CODECS_QUANT_SQRT_ENCODE_BINDING_H
