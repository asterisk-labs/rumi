#ifndef GEOZL_CODECS_QUANT_LOG_DECODE_BINDING_H
#define GEOZL_CODECS_QUANT_LOG_DECODE_BINDING_H

#include "graph_quant_log.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_quant_log(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_QUANT_LOG(id)                                                       \
  {                                                                            \
    .gd = QUANT_LOG_GRAPH, .transform_f = DI_geozl_quant_log,                  \
    .name = "geozl.lossy.quant_log",                                           \
  }

#endif // GEOZL_CODECS_QUANT_LOG_DECODE_BINDING_H
