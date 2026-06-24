#ifndef RUMI_CODECS_DELTA_W_ENCODE_BINDING_H
#define RUMI_CODECS_DELTA_W_ENCODE_BINDING_H

#include "graph_delta_w.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report RUMI_deltaW_encode(ZL_Encoder* eictx, const ZL_Input* in);

// Register with ZL_Compressor_registerTypedEncoder. The tile width is attached
// as local int param DELTAW_PARAM_WIDTH when the node is built.
static const ZL_TypedEncoderDesc DELTAW_encoder_desc = {
    .gd          = DELTAW_GRAPH,
    .transform_f = RUMI_deltaW_encode,
    .name        = "rumi.delta_w",
};

#endif // RUMI_CODECS_DELTA_W_ENCODE_BINDING_H
