#ifndef RUMI_CODECS_DELTA_W_DECODE_BINDING_H
#define RUMI_CODECS_DELTA_W_DECODE_BINDING_H

#include "graph_delta_w.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_rumi_delta_w(ZL_Decoder* dictx, const ZL_Input* ins[]);

// Register with ZL_DCtx_registerTypedDecoder, same CTid as the encoder.
static const ZL_TypedDecoderDesc delta_w_decoder_desc = {
    .gd          = DELTA_W_GRAPH,
    .transform_f = DI_rumi_delta_w,
    .name        = "rumi.experimental.delta_w",
};

#endif // RUMI_CODECS_DELTA_W_DECODE_BINDING_H
