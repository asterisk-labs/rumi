#ifndef RUMI_CODECS_DELTA_N_ENCODE_BINDING_H
#define RUMI_CODECS_DELTA_N_ENCODE_BINDING_H

#include "graph_delta_n.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_rumi_delta_n(ZL_Encoder* eictx, const ZL_Input* in);

static const ZL_TypedEncoderDesc delta_n_encoder_desc = {
    .gd          = DELTA_N_GRAPH,
    .transform_f = EI_rumi_delta_n,
    .name        = "rumi.experimental.delta_n",
};

#endif // RUMI_CODECS_DELTA_N_ENCODE_BINDING_H
