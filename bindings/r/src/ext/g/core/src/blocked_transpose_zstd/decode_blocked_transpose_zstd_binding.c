#include "decode_blocked_transpose_zstd_binding.h"

#include "blocked_transpose_zstd.h"
#include "decode_blocked_transpose_zstd_kernel.h"

#include "common/endian.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif
#include <zstd.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

enum { BTZ_OK = 0, BTZ_INVALID = 1, BTZ_ALLOC = 2, BTZ_ZSTD = 3 };

static int width_ok(size_t eltWidth) {
  return eltWidth == 1 || eltWidth == 2 || eltWidth == 4 || eltWidth == 8;
}

static int decode_with_dctx(ZSTD_DCtx *dctx, void *scratch, void *dst0,
                            size_t nbElts, size_t eltWidth, size_t blockSize,
                            const void *src0, size_t srcSize) {
  if (dctx == NULL || !width_ok(eltWidth) || blockSize < eltWidth ||
      blockSize > BLOCKED_TRANSPOSE_ZSTD_MAX_BLOCK_SIZE ||
      blockSize % eltWidth != 0 || nbElts > SIZE_MAX / eltWidth ||
      (nbElts != 0 && (dst0 == NULL || src0 == NULL)) ||
      (nbElts != 0 && eltWidth > 1 && scratch == NULL))
    return BTZ_INVALID;

  size_t zr = ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
  if (ZSTD_isError(zr))
    return BTZ_ZSTD;
  zr = ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
  if (ZSTD_isError(zr))
    return BTZ_ZSTD;

  uint8_t *dst = (uint8_t *)dst0;
  const uint8_t *src = (const uint8_t *)src0;
  const size_t totalSize = nbElts * eltWidth;
  size_t rawAt = 0;
  size_t at = 0;
  while (rawAt < totalSize) {
    if (srcSize - at < 4)
      return BTZ_INVALID;
    const size_t cSize = geozl_ld_le32(src + at);
    at += 4;
    if (cSize == 0 || cSize > srcSize - at)
      return BTZ_INVALID;
    const size_t left = totalSize - rawAt;
    const size_t rawSize = left < blockSize ? left : blockSize;
    void *zdst = eltWidth == 1 ? dst + rawAt : scratch;
    const size_t dSize =
        ZSTD_decompressDCtx(dctx, zdst, rawSize, src + at, cSize);
    if (ZSTD_isError(dSize) || dSize != rawSize)
      return BTZ_ZSTD;
    if (eltWidth > 1)
      blocked_transpose_zstd_unshuffle(dst + rawAt, scratch,
                                       rawSize / eltWidth, eltWidth);
    at += cSize;
    rawAt += rawSize;
  }
  return at == srcSize ? BTZ_OK : BTZ_INVALID;
}

GEOZL_API int geozl_blocked_transpose_zstd_decode(
    void *dst, size_t nbElts, size_t eltWidth, size_t blockSize,
    const void *src, size_t srcSize) {
  if (!width_ok(eltWidth) || blockSize < eltWidth ||
      blockSize > BLOCKED_TRANSPOSE_ZSTD_MAX_BLOCK_SIZE ||
      blockSize % eltWidth != 0 || nbElts > SIZE_MAX / eltWidth)
    return BTZ_INVALID;
  const size_t totalSize = nbElts * eltWidth;
  void *scratch = NULL;
  if (totalSize != 0 && eltWidth > 1) {
    const size_t scratchSize = totalSize < blockSize ? totalSize : blockSize;
    scratch = malloc(scratchSize);
    if (scratch == NULL)
      return BTZ_ALLOC;
  }
  ZSTD_DCtx *dctx = ZSTD_createDCtx();
  if (dctx == NULL) {
    free(scratch);
    return BTZ_ALLOC;
  }
  const int rc = decode_with_dctx(dctx, scratch, dst, nbElts, eltWidth,
                                  blockSize, src, srcSize);
  ZSTD_freeDCtx(dctx);
  free(scratch);
  return rc;
}

ZL_Report DI_geozl_blocked_transpose_zstd(ZL_Decoder *dictx,
                                           const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL && ins[0] != NULL);
  const ZL_Input *in = ins[0];
  assert(ZL_Input_type(in) == ZL_Type_serial);

  const ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != BLOCKED_TRANSPOSE_ZSTD_HEADER_SIZE)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint8_t *h = (const uint8_t *)header.start;
  if (h[0] != BLOCKED_TRANSPOSE_ZSTD_VERSION || h[2] != 0 || h[3] != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t eltWidth = h[1];
  const uint32_t blockSize = geozl_ld_le32(h + 4);
  const uint64_t count64 = geozl_ld_le64(h + 8);
  if (!width_ok(eltWidth) || blockSize < eltWidth ||
      blockSize > BLOCKED_TRANSPOSE_ZSTD_MAX_BLOCK_SIZE ||
      blockSize % eltWidth != 0 || count64 > SIZE_MAX / eltWidth)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t nbElts = (size_t)count64;
  const size_t srcSize = ZL_Input_numElts(in);
  if ((nbElts == 0) != (srcSize == 0))
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);
  const int rc = geozl_blocked_transpose_zstd_decode(
      ZL_Output_ptr(out), nbElts, eltWidth, blockSize, ZL_Input_ptr(in),
      srcSize);
  if (rc != BTZ_OK)
    return ZL_returnError(rc == BTZ_ALLOC ? ZL_ErrorCode_allocation
                                         : ZL_ErrorCode_corruption);
  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
