#ifndef RUMI_CODECS_DELTA_W_ENCODE_BINDING_H
#define RUMI_CODECS_DELTA_W_ENCODE_BINDING_H

#include "graph_delta_w.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_rumi_delta_w(ZL_Encoder* eictx, const ZL_Input* in);

// Register with ZL_Compressor_registerTypedEncoder. The tile width is attached
// as local int param DELTA_W_PARAM_WIDTH when the node is built.
static const ZL_TypedEncoderDesc delta_w_encoder_desc = {
    .gd          = DELTA_W_GRAPH,
    .transform_f = EI_rumi_delta_w,
    .name        = "rumi.experimental.delta_w",
};

#endif // RUMI_CODECS_DELTA_W_ENCODE_BINDING_H
