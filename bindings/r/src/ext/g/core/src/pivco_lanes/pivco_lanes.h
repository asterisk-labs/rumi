#ifndef GEOZL_CODECS_PIVCO_LANES_H
#define GEOZL_CODECS_PIVCO_LANES_H

#include "common/raster.h"

#include "openzl/zl_ctransform.h"
#include "openzl/zl_dtransform.h"

#include <stddef.h>
#include <stdint.h>

#define GEOZL_PIVCO_LANE_HEADER_SIZE (1 + 2 + 4)
#define GEOZL_PIVCO_LANE_PIVCO 0
#define GEOZL_PIVCO_LANE_PFOR 1
// OpenZL allocates the stream before checking the caller's output buffer.
#define GEOZL_PIVCO_LANES_MAX_BYTES ((size_t)256 << 20)

typedef struct {
  uint8_t mode;
  size_t weightsSize;
  size_t bitsSize;
} GeozlPivcoLane;

typedef struct {
  size_t count;
  size_t width;
  size_t blockSize;
  GeozlPivcoLane lane[8];
  size_t weightsSize;
  size_t bitsSize;
} GeozlPivcoLanes;

static inline int geozl_pivco_lanes_width_ok(size_t width) {
  return geozl_elt_width_ok(width);
}

static inline int geozl_pivco_lanes_size_ok(size_t count, size_t width) {
  return geozl_pivco_lanes_width_ok(width) &&
         count <= GEOZL_PIVCO_LANES_MAX_BYTES / width;
}

// lanes contains width contiguous byte streams, each count bytes long.
// The two output streams are committed here; laneHeaders receives one
// GEOZL_PIVCO_LANE_HEADER_SIZE record per lane.
ZL_Report geozl_pivco_lanes_encode(ZL_Encoder *encoder, const uint8_t *lanes,
                                   size_t count, size_t width, size_t blockSize,
                                   uint8_t *laneHeaders);

// Parse and bound a set of lane records before the caller allocates output.
int geozl_pivco_lanes_parse(GeozlPivcoLanes *decoded,
                            const uint8_t *laneHeaders, size_t laneHeadersSize,
                            size_t count, size_t width, size_t blockSize,
                            const uint8_t *weights, size_t weightsSize,
                            size_t bitsSize);

ZL_Report geozl_pivco_lanes_decode(ZL_Decoder *decoder,
                                   const GeozlPivcoLanes *header,
                                   const uint8_t *weights, const uint8_t *bits,
                                   uint8_t *lanes);

#endif // GEOZL_CODECS_PIVCO_LANES_H
