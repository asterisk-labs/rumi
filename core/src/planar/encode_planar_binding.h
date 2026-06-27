#ifndef RUMI_CODECS_PLANAR_ENCODE_BINDING_H
#define RUMI_CODECS_PLANAR_ENCODE_BINDING_H

#include "graph_planar.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_rumi_planar(ZL_Encoder* eictx, const ZL_Input* in);

static const ZL_TypedEncoderDesc planar_encoder_desc = {
    .gd          = PLANAR_GRAPH,
    .transform_f = EI_rumi_planar,
    .name        = "rumi.experimental.planar",
};

#endif // RUMI_CODECS_PLANAR_ENCODE_BINDING_H
