#ifndef GEOZL_CODECS_AVERAGE_ENCODE_BINDING_H
#define GEOZL_CODECS_AVERAGE_ENCODE_BINDING_H

#include "common/graph_num1to1.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_average(ZL_Encoder *eictx, const ZL_Input *in);

// Descriptor for ZL_Compressor_registerTypedEncoder.

#define EI_AVERAGE(id)                                                         \
  {                                                                            \
    .gd = GEOZL_NUM1TO1_GRAPH(id), .transform_f = EI_geozl_average,            \
    .name = "geozl.lossless.average",                                          \
  }

#endif // GEOZL_CODECS_AVERAGE_ENCODE_BINDING_H
