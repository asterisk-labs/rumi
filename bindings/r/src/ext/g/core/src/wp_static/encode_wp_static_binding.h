#ifndef GEOZL_CODECS_WP_STATIC_ENCODE_BINDING_H
#define GEOZL_CODECS_WP_STATIC_ENCODE_BINDING_H

#include "common/graph_num1to1.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_wp_static(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_WP_STATIC(id)                                                       \
  {                                                                            \
    .gd = GEOZL_NUM1TO1_GRAPH(id), .transform_f = EI_geozl_wp_static,          \
    .name = "geozl.lossless.wp_static",                                        \
  }

#endif // GEOZL_CODECS_WP_STATIC_ENCODE_BINDING_H
