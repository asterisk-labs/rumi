#ifndef GEOZL_H
#define GEOZL_H

#include "geozl/coeffs.h"
#include "geozl/ctids.h"
#include "geozl/dtype.h"
#include "geozl/export.h"
#include "geozl/quant_linear_params.h"
#include "geozl/quant_log_params.h"
#include "geozl/quant_sqrt_params.h"

#include "openzl/zl_compressor.h"
#include "openzl/zl_ctransform.h"
#include "openzl/zl_dtransform.h"
#include "openzl/zl_errors.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

// Register all geozl decoders in dctx.
GEOZL_API ZL_Report geozl_register_decoders(ZL_DCtx *dctx);

// Return nonzero when ctid is in geozl's reserved range.
GEOZL_API int geozl_owns_ctid(uint32_t ctid);
// Return nonzero when ctid is in geozl's lossy range.
GEOZL_API int geozl_ctid_is_lossy(uint32_t ctid);

// Build encoder nodes. width is in samples. planes is the number of stacked
// images; predictors that read the previous row reset between planes. Invalid
// plane layouts are treated as one plane. Returns ZL_NODE_ILLEGAL on failure.
GEOZL_API ZL_NodeID geozl_node_delta_w(ZL_Compressor *c, uint32_t width);
GEOZL_API ZL_NodeID geozl_node_delta_n(ZL_Compressor *c, uint32_t width,
                                       uint32_t planes);
GEOZL_API ZL_NodeID geozl_node_planar(ZL_Compressor *c, uint32_t width,
                                      uint32_t planes);
GEOZL_API ZL_NodeID geozl_node_planar_zigzag(ZL_Compressor *c, uint32_t width,
                                              uint32_t planes);
GEOZL_API ZL_NodeID geozl_node_planar_zigzag_pfor(ZL_Compressor *c,
                                                  uint32_t width,
                                                  uint32_t planes);
// Planar, Zigzag, byte transpose and PivCo or PFOR per residual byte lane.
GEOZL_API ZL_NodeID geozl_node_planar_zigzag_transpose_pivco(ZL_Compressor *c,
                                                             uint32_t width,
                                                             uint32_t planes);
// Source-compatible name for the same fused codec.
GEOZL_API ZL_NodeID geozl_node_planar_zigzag_pivco(ZL_Compressor *c,
                                                   uint32_t width,
                                                   uint32_t planes);
// Byte transpose followed by PivCo or PFOR independently on every lane.
GEOZL_API ZL_NodeID geozl_node_transpose_pivco(ZL_Compressor *c);
GEOZL_API ZL_NodeID geozl_node_med(ZL_Compressor *c, uint32_t width,
                                   uint32_t planes);
GEOZL_API ZL_NodeID geozl_node_med_zigzag(ZL_Compressor *c, uint32_t width,
                                          uint32_t planes);
GEOZL_API ZL_NodeID geozl_node_average(ZL_Compressor *c, uint32_t width,
                                       uint32_t planes);
GEOZL_API ZL_NodeID geozl_node_wp_static(ZL_Compressor *c, uint32_t width,
                                         uint32_t planes);
GEOZL_API ZL_NodeID geozl_node_deinterleave(ZL_Compressor *c);
GEOZL_API ZL_NodeID geozl_node_pfor(ZL_Compressor *c);
// Retired writers. These remain for source and link compatibility and return
// ZL_NODE_ILLEGAL.
GEOZL_API ZL_NodeID geozl_node_binoffset(ZL_Compressor *c);
GEOZL_API ZL_NodeID geozl_node_intmult(ZL_Compressor *c, uint64_t base);
GEOZL_API ZL_NodeID geozl_node_floatquant(ZL_Compressor *c, unsigned k);
GEOZL_API ZL_NodeID geozl_node_floatmult(ZL_Compressor *c, double base);

// params must be parsed and resolved before the node is built.
GEOZL_API ZL_NodeID geozl_node_quant_linear(ZL_Compressor *c,
                                            const quant_linear_params *params,
                                            int dtype);
GEOZL_API ZL_NodeID geozl_node_quant_log(ZL_Compressor *c,
                                         const quant_log_params *params,
                                         int dtype);
GEOZL_API ZL_NodeID geozl_node_quant_sqrt(ZL_Compressor *c,
                                          const quant_sqrt_params *params,
                                          int dtype);

// Missing-data handling. NAN applies to floating-point data; VALUE uses the
// caller-provided sentinel bits.
typedef enum {
  GEOZL_NODATA_NONE = 0,
  GEOZL_NODATA_NAN = 1,
  GEOZL_NODATA_VALUE = 2
} geozl_nodata_mode;

// Build a nodata node. Sentinel mode uses dtype to guard values that lossy
// stages may move onto valueBits. radius is their maximum error; use 0 for a
// lossless path and INFINITY when it is unknown. In NaN mode a radius of 0
// keeps every NaN's bits, and any other marks every NaN as a hole.
GEOZL_API ZL_NodeID geozl_node_nodata(ZL_Compressor *c, uint32_t width,
                                      geozl_nodata_mode mode,
                                      uint64_t valueBits, int dtype,
                                      double radius);

// Compress a raster with a graph recipe such as
// "planar>zigzag>transpose>entropy". error accepts:
//
//   NULL or ""                 lossless
//   "LINEAR:MAX_ERROR=V"       |x - x^| <= V
//   "LOG:MAX_ERROR=P%"         |x - x^| <= (P/100) * |x|
//   "SQRT:MAX_ERROR=VN"        |x - x^| <= V * sqrt(a + b*x)
//
// A SQRT recipe without A and B fits src. Supply both values to reuse one grid
// across tiles. nodataBits contains the sentinel's low eltWidth bytes and is
// read only for GEOZL_NODATA_VALUE.
// Returns 0 on success; otherwise a ZL_ErrorCode with details in errCtx.
GEOZL_API int geozl_2d_compress_c(const char *method, uint32_t width,
                                  uint32_t planes,
                                  const char *error, int dtype, int nodataMode,
                                  uint64_t nodataBits, const void *src,
                                  size_t numElts, size_t eltWidth, void *dst,
                                  size_t dstCapacity, size_t *outSize,
                                  char *errCtx, size_t errCtxSize);

// Reusable graph for tiles with the same configuration. src fixes the lossy
// domain; later tiles outside it are refused before compression.
typedef struct geozl_2d_graph_s geozl_2d_graph;

GEOZL_API int geozl_2d_graph_open_c(geozl_2d_graph **out, const char *method,
                                    uint32_t width, uint32_t planes,
                                    const char *error,
                                    int dtype, int nodataMode,
                                    uint64_t nodataBits, const void *src,
                                    size_t numElts, size_t eltWidth,
                                    char *errCtx, size_t errCtxSize);

// Compress one tile with an open graph. The element width comes from the graph.
// A declared nodata value is excluded from the lossy domain check.
GEOZL_API int geozl_2d_compress_graph_c(geozl_2d_graph *g, const void *src,
                                        size_t numElts, void *dst,
                                        size_t dstCapacity, size_t *outSize,
                                        char *errCtx, size_t errCtxSize);

// Free a graph. Accepts NULL.
GEOZL_API void geozl_2d_graph_close_c(geozl_2d_graph *g);

// Return the decompressed byte size, or 0 for an unreadable frame.
GEOZL_API size_t geozl_2d_frame_dsize_c(const void *frame, size_t frameSize);

// Decompress a frame into dst. Set verify to 0 to skip checksum verification.
// Returns 0 on success; otherwise a ZL_ErrorCode with details in errCtx.
GEOZL_API int geozl_2d_decompress_c(const void *frame, size_t frameSize,
                                    void *dst, size_t dstCapacity,
                                    size_t *outSize, int verify, char *errCtx,
                                    size_t errCtxSize);

// Benchmark one graph for reps round trips. checksum controls frame checksums;
// verify controls verification during decode. Returns the first error.
GEOZL_API int geozl_2d_bench_c(const char *method, uint32_t width,
                               uint32_t planes,
                               const char *error, int dtype, int nodataMode,
                               uint64_t nodataBits, const void *src,
                               size_t numElts, size_t eltWidth, size_t reps,
                               int checksum, int verify, size_t *compSize,
                               double *encSec, double *decSec, char *errCtx,
                               size_t errCtxSize);

// Compress one tile and attach a blob produced by geozl_coeffs_pack. Passing
// NULL and zero writes an unannotated frame. Coefficients apply to one call and
// do not become part of the graph.
GEOZL_API int geozl_2d_compress_coeffs_c(geozl_2d_graph *g, const void *src,
                                         size_t numElts, const void *coeffs,
                                         size_t coeffsSize, void *dst,
                                         size_t dstCapacity, size_t *outSize,
                                         char *errCtx, size_t errCtxSize);

// Copy a frame's coefficient blob without decompressing the payload. Return 0
// on success, -1 when absent, or a positive ZL_ErrorCode on failure. Call with
// dst NULL to query the size.
GEOZL_API int geozl_2d_frame_coeffs_c(const void *frame, size_t frameSize,
                                      void *dst, size_t dstCapacity,
                                      size_t *outSize);

// Expand method into recipe names, one per stride-byte slot. Returns -1 for an
// unknown method and writes the number of names to outCount.
GEOZL_API int geozl_2d_grid_c(const char *method, size_t eltWidth, char *names,
                              size_t stride, size_t maxNames, size_t *outCount);

#if defined(__cplusplus)
}
#endif

#endif // GEOZL_H
