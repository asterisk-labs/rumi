#include "encode_pfor_binding.h"
#include "encode_pfor_kernel.h"
#include "pfor_check.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>

ZL_Report EI_geozl_pfor(ZL_Encoder *eictx, const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);
  if (!geozl_pfor_width_ok(eltWidth))
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  uint8_t header[PFOR_HEADER_SIZE];
  geozl_st_le64(header, (uint64_t)nbElts);
  header[8] = (uint8_t)eltWidth;
  ZL_Encoder_sendCodecHeader(eictx, header, sizeof(header));

  if (nbElts == 0) {
    ZL_Output *empty = ZL_Encoder_createTypedStream(eictx, 0, 1, 1);
    ZL_ERR_IF_NULL(empty, allocation);
    ZL_ERR_IF_ERR(ZL_Output_commit(empty, 0));
    return ZL_returnSuccess();
  }

  const size_t bound = pfor_bound(nbElts, eltWidth);
  if (bound == 0)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  ZL_Output *out = ZL_Encoder_createTypedStream(eictx, 0, bound, 1);
  ZL_ERR_IF_NULL(out, allocation);

  size_t written = 0;
  if (pfor_encode(ZL_Output_ptr(out), bound, &written, ZL_Input_ptr(in), nbElts,
                  eltWidth) != 0)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, written));
  return ZL_returnSuccess();
}
