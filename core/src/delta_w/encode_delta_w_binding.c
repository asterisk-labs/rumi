#include "encode_delta_w_binding.h"
#include "encode_delta_w_kernel.h"

#include "openzl/zl_ctransform.h" // ZL_Encoder
#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include <assert.h>
#include <stdint.h>

ZL_Report RUMI_deltaW_encode(ZL_Encoder* eictx, const ZL_Input* in)
{
    assert(in != NULL);
    assert(ZL_Input_type(in) == ZL_Type_numeric);

    const size_t eltWidth = ZL_Input_eltWidth(in);
    const size_t nbElts   = ZL_Input_numElts(in);

    // the rumi graph builder attaches the tile row width as a local int param
    ZL_IntParam wp     = ZL_Encoder_getLocalIntParam(eictx, DELTAW_PARAM_WIDTH);
    const uint32_t wid = (wp.paramId == DELTAW_PARAM_WIDTH)
            ? (uint32_t)wp.paramValue
            : (uint32_t)nbElts;
    ZL_Encoder_sendCodecHeader(eictx, &wid, sizeof(wid));

    ZL_Output* out = ZL_Encoder_createTypedStream(eictx, 0, nbElts, eltWidth);
    if (out == NULL)
        return ZL_returnError(ZL_ErrorCode_allocation);

    DELTAW_encode(ZL_Output_ptr(out), ZL_Input_ptr(in), wid, nbElts, eltWidth);

    if (ZL_isError(ZL_Output_commit(out, nbElts)))
        return ZL_returnError(ZL_ErrorCode_GENERIC);
    return ZL_returnSuccess();
}
