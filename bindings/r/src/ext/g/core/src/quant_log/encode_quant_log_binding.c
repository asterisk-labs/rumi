#include "encode_quant_log_binding.h"

#include "encode_quant_log_kernel.h" // quant_log_encode
#include "graph_quant_log.h"         // QUANT_LOG_PARAM_*, QUANT_LOG_HEADER_SIZE
#include "quant_log_dtype.h"         // dtype codes and the width table

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ZL_Report EI_geozl_quant_log(ZL_Encoder *eictx, const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);

  // The parameters arrive resolved. The scan that decides the refusals and the
  // floor flag runs over the whole raster, and this node sees one stream of it.
  ZL_IntParam dp = ZL_Encoder_getLocalIntParam(eictx, QUANT_LOG_PARAM_DTYPE);
  ZL_CopyParam pp = ZL_Encoder_getLocalCopyParam(eictx, QUANT_LOG_PARAM_PARAMS);
  if (dp.paramId != QUANT_LOG_PARAM_DTYPE ||
      pp.paramId != QUANT_LOG_PARAM_PARAMS ||
      pp.paramSize != sizeof(quant_log_params))
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  const int dtype = dp.paramValue;
  quant_log_params p;
  memcpy(&p, pp.paramPtr, sizeof(p));

  if (!QLOG_DTYPE_OK(dtype) || quant_log_width(dtype) != eltWidth)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  ZL_Output *out = ZL_Encoder_createTypedStream(eictx, 0, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  // The resolver refused anything the grid cannot serve, so a failure here is
  // parameters this codec's own kernels reject, which is a bug rather than a
  // recipe the caller has to loosen.
  if (quant_log_encode(ZL_Output_ptr(out), ZL_Input_ptr(in), &p, dtype,
                       nbElts) != 0)
    ZL_ERR(GENERIC, "quant_log resolved parameters its own kernels reject");

  uint8_t header[QUANT_LOG_HEADER_SIZE];
  header[0] = (uint8_t)dtype;
  header[1] = p.flags;
  geozl_st_le_f64(header + 2, p.step);
  ZL_Encoder_sendCodecHeader(eictx, header, sizeof(header));

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
