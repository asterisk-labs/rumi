#include "decode_binoffset_binding.h"
#include "decode_binoffset_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ZL_Report DI_geozl_binoffset(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *bins = ins[0];
  const ZL_Input *offs = ins[1];
  assert(bins != NULL && offs != NULL);
  assert(ZL_Input_type(bins) == ZL_Type_numeric);
  assert(ZL_Input_type(offs) == ZL_Type_numeric);

  if (ZL_Input_eltWidth(bins) != 1)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t eltWidth = ZL_Input_eltWidth(offs);
  if (eltWidth != 1 && eltWidth != 2 && eltWidth != 4 && eltWidth != 8)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t nbElts = ZL_Input_numElts(bins);
  if (ZL_Input_numElts(offs) != nbElts)
    return ZL_returnError(ZL_ErrorCode_corruption);

  // header: uint8 nbBins in 1..=255, then nbBins x { lower : eltWidth, obits :
  // uint8 }
  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size < 1)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint8_t *hb = (const uint8_t *)header.start;
  const unsigned nbBins = hb[0];
  if (nbBins == 0 || header.size != 1 + (size_t)nbBins * (eltWidth + 1))
    return ZL_returnError(ZL_ErrorCode_corruption);

  // 256-entry tables so a corrupt bin id loads in bounds; padded obits stay 0
  uint64_t lowers[256];
  uint8_t obits[256];
  memset(lowers, 0, sizeof(lowers));
  memset(obits, 0, sizeof(obits));
  const uint8_t *e = hb + 1;
  for (unsigned b = 0; b < nbBins; ++b) {
    const uint64_t lower = geozl_ld_le(e, eltWidth);
    const unsigned ob = e[eltWidth];
    if (ob > 8u * eltWidth)
      return ZL_returnError(ZL_ErrorCode_corruption);
    lowers[b] = lower;
    obits[b] = (uint8_t)ob;
    e += eltWidth + 1;
  }

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  if (binoffset_join_table(ZL_Output_ptr(out), ZL_Input_ptr(bins),
                           ZL_Input_ptr(offs), nbElts, eltWidth, lowers, obits,
                           nbBins))
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
