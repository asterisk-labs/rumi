#include "pivco_lanes.h"

#include "common/endian.h"
#include "pfor/decode_pfor_kernel.h"
#include "pfor/encode_pfor_kernel.h"
#include "pfor/pfor_check.h"

#include "openzl/zl_errors.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

// PivCo has no public kernel API; GeoZL is built against a pinned OpenZL.
#define HUF_STATIC_LINKING_ONLY
#include "openzl/codecs/pivco_huffman/common_pivco_kernel.h"
#include "openzl/codecs/pivco_huffman/decode_pivco_kernel.h"
#include "openzl/codecs/pivco_huffman/encode_pivco_kernel.h"
#include "openzl/fse/huf.h"
#include "openzl/shared/histogram.h"

#include <limits.h>
#include <string.h>

int geozl_pivco_lanes_width_ok(size_t width) {
  return width == 1 || width == 2 || width == 4 || width == 8;
}

static int lane_weights(uint8_t weights[ZL_PIVCO_MAX_SYMBOLS],
                        size_t *weightsSize, const uint8_t *src, size_t count) {
  memset(weights, 0, ZL_PIVCO_MAX_SYMBOLS);
  ZL_Histogram8 hist;
  ZL_Histogram_init(&hist.base, 255);
  ZL_Histogram_build(&hist.base, src, count, 1);
  const unsigned maxSymbol = hist.base.maxSymbol;
  *weightsSize = (size_t)maxSymbol + 1;
  if (hist.base.cardinality == 1) {
    weights[maxSymbol] = 1;
    return 0;
  }

  HUF_CREATE_STATIC_CTABLE(ctable, HUF_SYMBOLVALUE_MAX);
  const unsigned log =
      HUF_optimalTableLog(ZL_PIVCO_MAX_TABLE_LOG, count, maxSymbol);
  const size_t built = HUF_buildCTable(ctable, hist.base.count, maxSymbol, log);
  if (HUF_isError(built) || built > ZL_PIVCO_MAX_TABLE_LOG)
    return -1;
  const unsigned tableLog = (unsigned)built;
  for (unsigned symbol = 0; symbol <= maxSymbol; ++symbol) {
    const unsigned bits = HUF_getNbBitsFromCTable(ctable, symbol);
    weights[symbol] = bits == 0 ? 0 : (uint8_t)(tableLog + 1 - bits);
  }
  return (int)tableLog;
}

static ZL_Report commit_empty(ZL_Encoder *encoder) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(encoder);
  for (int stream = 0; stream < 2; ++stream) {
    ZL_Output *out = ZL_Encoder_createTypedStream(encoder, stream, 0, 1);
    ZL_ERR_IF_NULL(out, allocation);
    ZL_ERR_IF_ERR(ZL_Output_commit(out, 0));
  }
  return ZL_returnSuccess();
}

ZL_Report geozl_pivco_lanes_encode(ZL_Encoder *encoder, const uint8_t *lanes,
                                   size_t count, size_t width, size_t blockSize,
                                   uint8_t *laneHeaders) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(encoder);
  if (encoder == NULL || laneHeaders == NULL ||
      !geozl_pivco_lanes_width_ok(width) || blockSize == 0 ||
      blockSize > ZL_PIVCO_MAX_BLOCK_SIZE || count > UINT_MAX ||
      (count != 0 && lanes == NULL))
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  memset(laneHeaders, 0, width * GEOZL_PIVCO_LANE_HEADER_SIZE);
  if (count == 0)
    return commit_empty(encoder);

  const size_t pivcoBound = ZL_PivCoHuffmanEncode_bound(count, blockSize);
  const size_t pforBound = pfor_bound(count, 1);
  if (pivcoBound == SIZE_MAX || pforBound == 0)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  const size_t laneBound = pivcoBound > pforBound ? pivcoBound : pforBound;
  if (laneBound > SIZE_MAX / width)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  uint8_t *pforTry = ZL_Encoder_getScratchSpace(encoder, pforBound);
  ZL_ERR_IF_NULL(pforTry, allocation);
  ZL_Output *weightsOut =
      ZL_Encoder_createTypedStream(encoder, 0, width * ZL_PIVCO_MAX_SYMBOLS, 1);
  ZL_ERR_IF_NULL(weightsOut, allocation);
  ZL_Output *bitsOut =
      ZL_Encoder_createTypedStream(encoder, 1, laneBound * width, 1);
  ZL_ERR_IF_NULL(bitsOut, allocation);
  const size_t scratchSize =
      ZL_PivCoHuffmanEncode_scratchElements(count, blockSize);
  uint8_t *scratch = ZL_Encoder_getScratchSpace(encoder, scratchSize);
  ZL_ERR_IF_NULL(scratch, allocation);

  uint8_t *weightsDst = ZL_Output_ptr(weightsOut);
  uint8_t *bitsDst = ZL_Output_ptr(bitsOut);
  size_t weightsTotal = 0;
  size_t bitsTotal = 0;
  for (size_t laneIndex = 0; laneIndex < width; ++laneIndex) {
    const uint8_t *lane = lanes + laneIndex * count;
    uint8_t weights[ZL_PIVCO_MAX_SYMBOLS];
    size_t weightsSize = 0;
    const int tableLog = lane_weights(weights, &weightsSize, lane, count);
    if (tableLog < 0)
      return ZL_returnError(ZL_ErrorCode_GENERIC);

    size_t used = ZL_PivCoHuffman_encode(
        bitsDst + bitsTotal, pivcoBound, scratch, scratchSize, weights,
        weightsSize, tableLog, lane, count, blockSize, NULL);
    if (used == SIZE_MAX)
      return ZL_returnError(ZL_ErrorCode_GENERIC);

    uint8_t mode = GEOZL_PIVCO_LANE_PIVCO;
    size_t pforSize = 0;
    if (pfor_encode(pforTry, pforBound, &pforSize, lane, count, 1) != 0)
      return ZL_returnError(ZL_ErrorCode_GENERIC);
    if (weightsSize > SIZE_MAX - used || pforSize < used + weightsSize) {
      memcpy(bitsDst + bitsTotal, pforTry, pforSize);
      used = pforSize;
      weightsSize = 0;
      mode = GEOZL_PIVCO_LANE_PFOR;
    }
    if (used > UINT32_MAX)
      return ZL_returnError(ZL_ErrorCode_GENERIC);

    memcpy(weightsDst + weightsTotal, weights, weightsSize);
    uint8_t *laneHeader =
        laneHeaders + laneIndex * GEOZL_PIVCO_LANE_HEADER_SIZE;
    laneHeader[0] = mode;
    geozl_st_le16(laneHeader + 1, (uint16_t)weightsSize);
    geozl_st_le32(laneHeader + 3, (uint32_t)used);
    weightsTotal += weightsSize;
    bitsTotal += used;
  }

  ZL_ERR_IF_ERR(ZL_Output_commit(weightsOut, weightsTotal));
  ZL_ERR_IF_ERR(ZL_Output_commit(bitsOut, bitsTotal));
  return ZL_returnSuccess();
}

int geozl_pivco_lanes_parse(GeozlPivcoLanes *decoded,
                            const uint8_t *laneHeaders, size_t laneHeadersSize,
                            size_t count, size_t width, size_t blockSize,
                            const uint8_t *weights, size_t weightsSize,
                            size_t bitsSize) {
  if (decoded == NULL || laneHeaders == NULL ||
      !geozl_pivco_lanes_width_ok(width) ||
      laneHeadersSize != width * GEOZL_PIVCO_LANE_HEADER_SIZE ||
      blockSize == 0 || blockSize > ZL_PIVCO_MAX_BLOCK_SIZE ||
      (weightsSize != 0 && weights == NULL))
    return 1;

  *decoded = (GeozlPivcoLanes){
      .count = count,
      .width = width,
      .blockSize = blockSize,
  };
  for (size_t laneIndex = 0; laneIndex < width; ++laneIndex) {
    const uint8_t *src = laneHeaders + laneIndex * GEOZL_PIVCO_LANE_HEADER_SIZE;
    GeozlPivcoLane *lane = &decoded->lane[laneIndex];
    lane->mode = src[0];
    lane->weightsSize = geozl_ld_le16(src + 1);
    lane->bitsSize = geozl_ld_le32(src + 3);
    if (lane->mode > GEOZL_PIVCO_LANE_PFOR ||
        lane->weightsSize > ZL_PIVCO_MAX_SYMBOLS ||
        (lane->mode == GEOZL_PIVCO_LANE_PFOR && lane->weightsSize != 0) ||
        decoded->weightsSize > weightsSize ||
        lane->weightsSize > weightsSize - decoded->weightsSize ||
        decoded->bitsSize > bitsSize ||
        lane->bitsSize > bitsSize - decoded->bitsSize)
      return 1;
    decoded->weightsSize += lane->weightsSize;
    decoded->bitsSize += lane->bitsSize;
  }
  if (decoded->weightsSize != weightsSize || decoded->bitsSize != bitsSize)
    return 1;
  if (count == 0)
    return weightsSize != 0 || bitsSize != 0;

  size_t weightsOffset = 0;
  for (size_t laneIndex = 0; laneIndex < width; ++laneIndex) {
    const GeozlPivcoLane *lane = &decoded->lane[laneIndex];
    if (lane->mode == GEOZL_PIVCO_LANE_PFOR) {
      const size_t blocks =
          count / GEOZL_PFOR_BLOCK + (count % GEOZL_PFOR_BLOCK != 0);
      if (blocks > SIZE_MAX / 2 || lane->bitsSize < blocks * 2)
        return 1;
      continue;
    }
    if (lane->weightsSize == 0)
      return 1;
    size_t symbols = 0;
    for (size_t symbol = 0; symbol < lane->weightsSize; ++symbol)
      symbols += weights[weightsOffset + symbol] != 0;
    if (symbols == 0 ||
        (symbols > 1 && lane->bitsSize < count / 8 + (count % 8 != 0)))
      return 1;
    weightsOffset += lane->weightsSize;
  }
  return 0;
}

ZL_Report geozl_pivco_lanes_decode(ZL_Decoder *decoder,
                                   const GeozlPivcoLanes *header,
                                   const uint8_t *weights, const uint8_t *bits,
                                   uint8_t *lanes) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(decoder);
  if (decoder == NULL || header == NULL ||
      (header->count != 0 && lanes == NULL) ||
      (header->weightsSize != 0 && weights == NULL) ||
      (header->bitsSize != 0 && bits == NULL))
    return ZL_returnError(ZL_ErrorCode_corruption);
  if (header->count == 0)
    return ZL_returnSuccess();

  // A constant PivCo lane has weights but no bitstream. Keep pointer
  // arithmetic defined even when OpenZL represents that empty stream by NULL.
  static const uint8_t emptyBits = 0;
  if (bits == NULL)
    bits = &emptyBits;

  const size_t scratchSize =
      ZL_PivCoHuffmanDecode_scratchBytes(header->count, header->blockSize);
  uint8_t *scratch = ZL_Decoder_getScratchSpace(decoder, scratchSize);
  ZL_ERR_IF_NULL(scratch, allocation);

  size_t weightsOffset = 0;
  size_t bitsOffset = 0;
  for (size_t laneIndex = 0; laneIndex < header->width; ++laneIndex) {
    const GeozlPivcoLane *lane = &header->lane[laneIndex];
    uint8_t *dst = lanes + laneIndex * header->count;
    if (lane->mode == GEOZL_PIVCO_LANE_PFOR) {
      if (pfor_decode(dst, header->count, 1, bits + bitsOffset,
                      lane->bitsSize) != 0)
        return ZL_returnError(ZL_ErrorCode_corruption);
    } else if (!ZL_PivCoHuffman_decode(dst, header->count, scratch, scratchSize,
                                       weights + weightsOffset,
                                       lane->weightsSize, bits + bitsOffset,
                                       lane->bitsSize, header->blockSize,
                                       NULL)) {
      return ZL_returnError(ZL_ErrorCode_corruption);
    }
    weightsOffset += lane->weightsSize;
    bitsOffset += lane->bitsSize;
  }
  return ZL_returnSuccess();
}
