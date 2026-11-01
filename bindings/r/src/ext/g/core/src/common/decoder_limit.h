#ifndef GEOZL_COMMON_DECODER_LIMIT_H
#define GEOZL_COMMON_DECODER_LIMIT_H

#include "openzl/decompress/dctx2.h"
#include "openzl/decompress/dictx.h"
#include "openzl/zl_decompress.h"
#include "openzl/zl_errors.h"

#include <stddef.h>

// A forged codec header must not reserve more than the frame says it can
// produce. Otherwise OpenZL may create an internal stream when the caller's
// final output buffer is too small.
static inline int geozl_decoder_size_fits_frame(const ZL_Decoder *decoder,
                                                size_t bytes) {
  if (decoder == NULL || decoder->dctx == NULL)
    return 0;

  const DFH_Struct *header = DCtx_getFrameHeader(decoder->dctx);
  if (header == NULL || header->frameinfo == NULL)
    return 0;

  const ZL_Report outputs = ZL_FrameInfo_getNumOutputs(header->frameinfo);
  if (ZL_isError(outputs))
    return 0;

  size_t largest = 0;
  for (size_t i = 0; i < ZL_validResult(outputs); ++i) {
    const ZL_Report size =
        ZL_FrameInfo_getDecompressedSize(header->frameinfo, (int)i);
    if (ZL_isError(size))
      return 0;
    if (ZL_validResult(size) > largest)
      largest = ZL_validResult(size);
  }
  return bytes <= largest;
}

#endif // GEOZL_COMMON_DECODER_LIMIT_H
