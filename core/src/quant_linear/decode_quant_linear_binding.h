#ifndef RUMI_CODECS_QUANT_LINEAR_DECODE_BINDING_H
#define RUMI_CODECS_QUANT_LINEAR_DECODE_BINDING_H

#include "graph_quant_linear.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_rumi_quant_linear(ZL_Decoder* dictx, const ZL_Input* ins[]);

static const ZL_TypedDecoderDesc quant_linear_decoder_desc = {
    .gd          = QUANT_LINEAR_GRAPH,
    .transform_f = DI_rumi_quant_linear,
    .name        = "rumi.lossy.quant_linear",
};

#endif // RUMI_CODECS_QUANT_LINEAR_DECODE_BINDING_H
