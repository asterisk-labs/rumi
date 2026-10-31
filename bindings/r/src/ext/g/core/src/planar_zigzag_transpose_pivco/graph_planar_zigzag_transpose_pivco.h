#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_GRAPH_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_GRAPH_H

#include "pivco_lanes/pivco_lanes.h"

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

// One numeric input; out: the lanes' Huffman weights, 1-byte numeric, and the
// PivCo or PFOR payloads, serial.
#define PLANAR_ZIGZAG_TRANSPOSE_PIVCO_GRAPH(id)                                \
  {                                                                            \
    .CTid = (id), .inStreamType = ZL_Type_numeric,                             \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_numeric, ZL_Type_serial)       \
  }

// Little endian: uint64 element count, uint8 element width, uint32 row width,
// uint32 plane count, uint32 PivCo block size, then for each of the
// element-width lanes a uint8 mode, a uint16 weights size and a uint32
// bitstream size.
#define PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED (8 + 1 + 4 + 4 + 4)
#define PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_LANE GEOZL_PIVCO_LANE_HEADER_SIZE

// A lane is PivCo Huffman, or byte PFOR where that is smaller: Huffman spends
// at least a bit per value, so a lane that is almost one value goes to PFOR.
#define PLANAR_ZIGZAG_TRANSPOSE_PIVCO_LANE_PIVCO GEOZL_PIVCO_LANE_PIVCO
#define PLANAR_ZIGZAG_TRANSPOSE_PIVCO_LANE_PFOR GEOZL_PIVCO_LANE_PFOR
#define PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_MAX                               \
  (PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED +                                \
   8 * PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_LANE)

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_GRAPH_H
