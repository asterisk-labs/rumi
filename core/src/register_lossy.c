#include "register_lossy.h"

#include "quant_linear/decode_quant_linear_binding.h"

#include "openzl/zl_dtransform.h"
#include "openzl/zl_errors.h"

ZL_Report rumi_register_lossy_decoders(ZL_DCtx* dctx)
{
    ZL_Report r = ZL_DCtx_registerTypedDecoder(dctx, &quant_linear_decoder_desc);
    if (ZL_isError(r))
        return r;
    return ZL_returnSuccess();
}
