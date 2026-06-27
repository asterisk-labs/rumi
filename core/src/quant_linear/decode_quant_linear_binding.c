#include "decode_quant_linear_binding.h"
#include "decode_quant_linear_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

// Codec header, 9 bytes, little-endian:
//   byte 0      ql_dtype of the original stream
//   bytes 1..8  scale, IEEE double, scale = 2 * max_error
ZL_Report DI_rumi_quant_linear(ZL_Decoder* dictx, const ZL_Input* ins[])
{
    assert(ins != NULL);
    const ZL_Input* in = ins[0];
    assert(in != NULL);
    assert(ZL_Input_type(in) == ZL_Type_numeric);

    const size_t eltWidth = ZL_Input_eltWidth(in);
    const size_t nbElts   = ZL_Input_numElts(in);

    ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
    if (header.size != 1 + sizeof(double))
        return ZL_returnError(ZL_ErrorCode_corruption);
    const uint8_t* hb = (const uint8_t*)header.start;
    int    dtype = (int)hb[0];
    double scale;
    memcpy(&scale, hb + 1, sizeof(scale));

    // The index width equals the original element width for every supported type,
    // so the output stream keeps the input stream width.
    ZL_Output* out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
    if (out == NULL)
        return ZL_returnError(ZL_ErrorCode_allocation);

    quant_linear_decode(ZL_Output_ptr(out), ZL_Input_ptr(in), scale, dtype, nbElts);

    if (ZL_isError(ZL_Output_commit(out, nbElts)))
        return ZL_returnError(ZL_ErrorCode_GENERIC);
    return ZL_returnSuccess();
}
