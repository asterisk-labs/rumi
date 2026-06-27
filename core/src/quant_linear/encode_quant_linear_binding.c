#include "encode_quant_linear_binding.h"
#include "encode_quant_linear_kernel.h"

#include "openzl/zl_ctransform.h"
#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"

#include <stdint.h>
#include <string.h>

ZL_Report EI_rumi_quant_linear(ZL_Encoder* eictx, const ZL_Input* ins[], size_t nbIns)
{
    ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
    ZL_ASSERT_EQ(nbIns, 1);
    ZL_ASSERT_NN(ins);
    const ZL_Input* in    = ins[0];
    const size_t srcWidth = ZL_Input_eltWidth(in);
    const size_t nbElts   = ZL_Input_numElts(in);

    ZL_IntParam dp = ZL_Encoder_getLocalIntParam(eictx, QUANT_LINEAR_PARAM_DTYPE);
    ZL_IntParam sp = ZL_Encoder_getLocalIntParam(eictx, QUANT_LINEAR_PARAM_SCALE_BITS);
    ZL_ERR_IF_NE(dp.paramId, QUANT_LINEAR_PARAM_DTYPE, node_invalid_input);
    ZL_ERR_IF_NE(sp.paramId, QUANT_LINEAR_PARAM_SCALE_BITS, node_invalid_input);
    const int dtype = dp.paramValue;
    int64_t bits    = (int64_t)sp.paramValue;
    double scale;
    memcpy(&scale, &bits, sizeof(scale));

    // The index keeps the original element width.
    ZL_Output* out = ZL_Encoder_createTypedStream(eictx, 0, nbElts, srcWidth);
    ZL_ERR_IF(!out, allocation);
    quant_linear_encode(ZL_Output_ptr(out), ZL_Input_ptr(in), scale, dtype, nbElts);
    ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));

    uint8_t header[1 + sizeof(double)];
    header[0] = (uint8_t)dtype;
    memcpy(header + 1, &scale, sizeof(scale));
    ZL_Encoder_sendCodecHeader(eictx, header, sizeof(header));
    return ZL_returnValue(1);
}
