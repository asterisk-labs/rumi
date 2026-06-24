#include "register_experimental.h"

#include "delta_w/decode_delta_w_binding.h" // RUMI_deltaW_decode
#include "delta_w/graph_delta_w.h"          // DELTAW_GRAPH
#include "delta_n/decode_delta_n_binding.h"
#include "delta_n/graph_delta_n.h"
#include "planar/decode_planar_binding.h"
#include "planar/graph_planar.h"

#include "openzl/zl_errors.h"

// The registry copies the stream type array, so the local compound literal in
// each GRAPH macro is safe to pass by value here.
ZL_Report rumi_register_experimental_decoders(ZL_DCtx* dctx)
{
    {
        const ZL_TypedDecoderDesc d = {
            .gd          = DELTAW_GRAPH,
            .transform_f = RUMI_deltaW_decode,
            .name        = "rumi.delta_w",
        };
        const ZL_Report r = ZL_DCtx_registerTypedDecoder(dctx, &d);
        if (ZL_isError(r)) return r;
    }
    {
        const ZL_TypedDecoderDesc d = {
            .gd          = DELTAN_GRAPH,
            .transform_f = RUMI_deltaN_decode,
            .name        = "rumi.delta_n",
        };
        const ZL_Report r = ZL_DCtx_registerTypedDecoder(dctx, &d);
        if (ZL_isError(r)) return r;
    }
    {
        const ZL_TypedDecoderDesc d = {
            .gd          = PLANAR_GRAPH,
            .transform_f = RUMI_planar_decode,
            .name        = "rumi.planar",
        };
        const ZL_Report r = ZL_DCtx_registerTypedDecoder(dctx, &d);
        if (ZL_isError(r)) return r;
    }
    return ZL_returnSuccess();
}