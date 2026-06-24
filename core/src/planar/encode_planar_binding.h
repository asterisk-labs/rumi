#ifndef RUMI_CODECS_PLANAR_ENCODE_BINDING_H
#define RUMI_CODECS_PLANAR_ENCODE_BINDING_H

#include "graph_planar.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report RUMI_planar_encode(ZL_Encoder* eictx, const ZL_Input* in);

static const ZL_TypedEncoderDesc PLANAR_encoder_desc = {
    .gd          = PLANAR_GRAPH,
    .transform_f = RUMI_planar_encode,
    .name        = "rumi.planar",
};

#endif // RUMI_CODECS_PLANAR_ENCODE_BINDING_H
