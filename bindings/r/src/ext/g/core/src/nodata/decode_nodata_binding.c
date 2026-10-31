#include "decode_nodata_binding.h"
#include "decode_nodata_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>

ZL_Report DI_geozl_nodata(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *vals = ins[0];
  const ZL_Input *mask = ins[1];
  assert(vals != NULL && mask != NULL);
  assert(ZL_Input_type(vals) == ZL_Type_numeric);
  assert(ZL_Input_type(mask) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(vals);
  if (eltWidth != 1 && eltWidth != 2 && eltWidth != 4 && eltWidth != 8)
    return ZL_returnError(ZL_ErrorCode_corruption);
  if (ZL_Input_eltWidth(mask) != 1)
    return ZL_returnError(ZL_ErrorCode_corruption);

  // Header length distinguishes the plain and guarded forms.
  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  const int guarded = header.size == 4 * eltWidth;
  if (header.size != eltWidth && !guarded)
    return ZL_returnError(ZL_ErrorCode_corruption);

  const size_t nbElts = ZL_Input_numElts(vals);
  if (nbElts == 0 || ZL_Input_numElts(mask) != nbElts)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint8_t *h = (const uint8_t *)header.start;
  const uint64_t pattern = geozl_ld_le(h, eltWidth);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  if (guarded) {
    const uint64_t repl[3] = {geozl_ld_le(h + eltWidth, eltWidth),
                              geozl_ld_le(h + 2 * eltWidth, eltWidth),
                              geozl_ld_le(h + 3 * eltWidth, eltWidth)};
    if (nodata_restore_guarded(ZL_Output_ptr(out), ZL_Input_ptr(vals),
                               ZL_Input_ptr(mask), nbElts, eltWidth, pattern,
                               repl) != 0)
      return ZL_returnError(ZL_ErrorCode_corruption);
  } else {
    nodata_restore(ZL_Output_ptr(out), ZL_Input_ptr(vals), ZL_Input_ptr(mask),
                   nbElts, eltWidth, pattern);
  }

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
