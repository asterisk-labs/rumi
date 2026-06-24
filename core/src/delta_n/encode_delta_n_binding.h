#ifndef RUMI_CODECS_DELTA_N_ENCODE_BINDING_H
#define RUMI_CODECS_DELTA_N_ENCODE_BINDING_H

#include "graph_delta_n.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report RUMI_deltaN_encode(ZL_Encoder* eictx, const ZL_Input* in);

static const ZL_TypedEncoderDesc DELTAN_encoder_desc = {
    .gd          = DELTAN_GRAPH,
    .transform_f = RUMI_deltaN_encode,
    .name        = "rumi.delta_n",
};

#endif // RUMI_CODECS_DELTA_N_ENCODE_BINDING_H
