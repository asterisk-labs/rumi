#ifndef RUMI_CODECS_PLANAR_DECODE_BINDING_H
#define RUMI_CODECS_PLANAR_DECODE_BINDING_H

#include "graph_planar.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report RUMI_planar_decode(ZL_Decoder* dictx, const ZL_Input* ins[]);

static const ZL_TypedDecoderDesc PLANAR_decoder_desc = {
    .gd          = PLANAR_GRAPH,
    .transform_f = RUMI_planar_decode,
    .name        = "rumi.planar",
};

#endif // RUMI_CODECS_PLANAR_DECODE_BINDING_H
