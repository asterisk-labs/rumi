#ifndef RUMI_CODECS_DELTA_W_DECODE_BINDING_H
#define RUMI_CODECS_DELTA_W_DECODE_BINDING_H

#include "graph_delta_w.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report RUMI_deltaW_decode(ZL_Decoder* dictx, const ZL_Input* ins[]);

// Register with ZL_DCtx_registerTypedDecoder, same CTid as the encoder.
static const ZL_TypedDecoderDesc DELTAW_decoder_desc = {
    .gd          = DELTAW_GRAPH,
    .transform_f = RUMI_deltaW_decode,
    .name        = "rumi.delta_w",
};

#endif // RUMI_CODECS_DELTA_W_DECODE_BINDING_H
