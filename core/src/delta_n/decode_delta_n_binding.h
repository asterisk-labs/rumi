#ifndef RUMI_CODECS_DELTA_N_DECODE_BINDING_H
#define RUMI_CODECS_DELTA_N_DECODE_BINDING_H

#include "graph_delta_n.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report RUMI_deltaN_decode(ZL_Decoder* dictx, const ZL_Input* ins[]);

static const ZL_TypedDecoderDesc DELTAN_decoder_desc = {
    .gd          = DELTAN_GRAPH,
    .transform_f = RUMI_deltaN_decode,
    .name        = "rumi.delta_n",
};

#endif // RUMI_CODECS_DELTA_N_DECODE_BINDING_H
