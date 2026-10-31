#ifndef GEOZL_CODECS_QUANT_LINEAR_ENCODE_BINDING_H
#define GEOZL_CODECS_QUANT_LINEAR_ENCODE_BINDING_H

#include "graph_quant_linear.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_quant_linear(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_QUANT_LINEAR(id)                                                    \
  {                                                                            \
    .gd = QUANT_LINEAR_GRAPH, .transform_f = EI_geozl_quant_linear,            \
    .name = "geozl.lossy.quant_linear",                                        \
  }

#endif // GEOZL_CODECS_QUANT_LINEAR_ENCODE_BINDING_H
