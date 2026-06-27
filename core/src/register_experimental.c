#include "register_experimental.h"

#include "delta_w/decode_delta_w_binding.h"  // delta_w_decoder_desc
#include "delta_n/decode_delta_n_binding.h"  // delta_n_decoder_desc
#include "planar/decode_planar_binding.h"    // planar_decoder_desc

#include "openzl/zl_dtransform.h"
#include "openzl/zl_errors.h"

// The decoder descriptors live in each codec's decode binding header as a
// static const, so this just registers the ones already defined there.
ZL_Report rumi_register_experimental_decoders(ZL_DCtx* dctx)
{
    ZL_Report r = ZL_DCtx_registerTypedDecoder(dctx, &delta_w_decoder_desc);
    if (ZL_isError(r)) return r;
    r = ZL_DCtx_registerTypedDecoder(dctx, &delta_n_decoder_desc);
    if (ZL_isError(r)) return r;
    r = ZL_DCtx_registerTypedDecoder(dctx, &planar_decoder_desc);
    if (ZL_isError(r)) return r;
    return ZL_returnSuccess();
}
