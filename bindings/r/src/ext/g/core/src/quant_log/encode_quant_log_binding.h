#ifndef GEOZL_CODECS_QUANT_LOG_ENCODE_BINDING_H
#define GEOZL_CODECS_QUANT_LOG_ENCODE_BINDING_H

#include "graph_quant_log.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_quant_log(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_QUANT_LOG(id)                                                       \
  {                                                                            \
    .gd = QUANT_LOG_GRAPH, .transform_f = EI_geozl_quant_log,                  \
    .name = "geozl.lossy.quant_log",                                           \
  }

#endif // GEOZL_CODECS_QUANT_LOG_ENCODE_BINDING_H
