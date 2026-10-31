#ifndef GEOZL_CODECS_DELTA_N_ENCODE_BINDING_H
#define GEOZL_CODECS_DELTA_N_ENCODE_BINDING_H

#include "common/graph_num1to1.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_delta_n(ZL_Encoder *eictx, const ZL_Input *in);

// Descriptor for ZL_Compressor_registerTypedEncoder.

#define EI_DELTA_N(id)                                                         \
  {                                                                            \
    .gd = GEOZL_NUM1TO1_GRAPH(id), .transform_f = EI_geozl_delta_n,            \
    .name = "geozl.lossless.delta_n",                                          \
  }

#endif // GEOZL_CODECS_DELTA_N_ENCODE_BINDING_H
