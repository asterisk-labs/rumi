#include "decode_planar_zigzag_pfor_binding.h"
#include "decode_planar_zigzag_pfor_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"
#include "pfor/pfor_check.h"
#include "planar_zigzag_pfor_layout.h"

#include <assert.h>
#include <stdint.h>

ZL_Report DI_geozl_planar_zigzag_pfor(ZL_Decoder *dictx,
                                       const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *in = ins[0];
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_serial);

  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != PLANAR_ZIGZAG_PFOR_HEADER_SIZE)
    ZL_ERR(corruption, "header is %zu bytes, not %d", header.size,
           PLANAR_ZIGZAG_PFOR_HEADER_SIZE);
  const uint8_t *h = (const uint8_t *)header.start;
  const uint64_t nbElts = geozl_ld_le64(h);
  const size_t eltWidth = h[8];
  const uint32_t width = geozl_ld_le32(h + 9);
  const uint32_t planes = geozl_ld_le32(h + 13);
  if (!geozl_pfor_width_ok(eltWidth))
    ZL_ERR(corruption, "elements are %zu bytes wide", eltWidth);
  if (planes == 0)
    ZL_ERR(corruption, "header declares 0 planes");

  const size_t srcSize = ZL_Input_numElts(in);
  if (nbElts == 0) {
    if (srcSize != 0)
      ZL_ERR(corruption, "%zu payload bytes for 0 elements", srcSize);
    ZL_Output *empty = ZL_Decoder_create1OutStream(dictx, 0, eltWidth);
    ZL_ERR_IF_NULL(empty, allocation);
    ZL_ERR_IF_ERR(ZL_Output_commit(empty, 0));
    return ZL_returnSuccess();
  }

  if (width == 0)
    ZL_ERR(corruption, "header declares width 0");
  if (nbElts % planes != 0)
    ZL_ERR(corruption, "%llu elements are not %u equal planes",
           (unsigned long long)nbElts, planes);
  const uint64_t planeElts = nbElts / planes;
  if (width > planeElts || planeElts % width != 0)
    ZL_ERR(corruption, "planes of %llu elements are not rows of %u",
           (unsigned long long)planeElts, width);

  // Block headers bound the output allocation.
  if (nbElts > (uint64_t)(SIZE_MAX / eltWidth))
    ZL_ERR(corruption, "%llu elements overflow size_t",
           (unsigned long long)nbElts);
  const size_t perPlane =
      geozl_pzp_plane_blocks(width, (size_t)(planeElts / width));
  if (perPlane > srcSize / GEOZL_PZP_HEADER_BYTES / planes)
    ZL_ERR(corruption, "%zu payload bytes cannot hold %zu blocks per plane",
           srcSize, perPlane);
  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, (size_t)nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);
  if (planar_zigzag_pfor_decode(
          ZL_Output_ptr(out), width, (size_t)nbElts, eltWidth, planes,
          ZL_Input_ptr(in), srcSize) != 0)
    ZL_ERR(corruption, "blocks do not decode to %llu elements",
           (unsigned long long)nbElts);
  ZL_ERR_IF_ERR(ZL_Output_commit(out, (size_t)nbElts));
  return ZL_returnSuccess();
}
