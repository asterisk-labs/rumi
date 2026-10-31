#define _POSIX_C_SOURCE 200809L // clock_gettime

#include "geozl/coeffs.h"
#include "geozl/geozl.h"

#include "openzl/zl_compress.h"
#include "openzl/zl_compressor.h"
#include "openzl/zl_data.h"
#include "openzl/zl_decompress.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_graph_api.h"
#include "openzl/zl_input.h"   // ZL_Input_numElts and friends
#include "openzl/codecs/zl_constant.h"   // ZL_GRAPH_CONSTANT
#include "openzl/codecs/zl_conversion.h" // ZL_NODE_CONVERT_*
#include "openzl/codecs/zl_delta.h"      // ZL_NODE_DELTA_INT
#include "openzl/codecs/zl_entropy.h"    // ZL_GRAPH_ENTROPY
#include "openzl/codecs/zl_field_lz.h"   // ZL_GRAPH_FIELD_LZ
#include "openzl/codecs/zl_generic.h"    // ZL_GRAPH_NUMERIC
#include "openzl/codecs/zl_illegal.h"    // ZL_GRAPH_ILLEGAL
#include "openzl/codecs/zl_sparse_num.h" // ZL_NODE_SPARSE_NUM_AUTO
#include "openzl/codecs/zl_store.h"      // ZL_GRAPH_STORE
#include "openzl/codecs/zl_transpose.h"  // ZL_NODE_TRANSPOSE_SPLIT
#include "openzl/codecs/zl_zigzag.h"     // ZL_NODE_ZIGZAG
#include "openzl/codecs/zl_zstd.h"       // ZL_GRAPH_ZSTD

#include "common/fp.h"
#include "common/half.h"
#include "lossy/lossy_node.h"   // geozl_node_lossy
#include "lossy/lossy_recipe.h" // geozl_lossy_parse and friends

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // QueryPerformanceCounter
#endif

#define OPENZL_COMMENT_VERSION_MIN 22
// OpenZL 0.2 reads up to format 24, and nothing geozl writes gains from 27
// except PivCo Huffman and sparse_num, which exist from 27 and 26 on.
#define GEOZL_FORMAT_VERSION 24
#define GEOZL_OPENZL03_FORMAT_VERSION 27

// A method combines an optional predictor with a terminal, for example
// "planar>zigzag>entropy". The full graph is:
//
//   [nodata] -> [quantizer] -> [predictor -> zigzag] -> terminal
//
// nodata runs before quantization. Frames are self-describing on decode.

// ID and DELTA_1D do not use zigzag.
typedef enum {
  GEOZL_PRED_DELTA_W = 0,
  GEOZL_PRED_DELTA_N,
  GEOZL_PRED_PLANAR,
  GEOZL_PRED_MED,
  GEOZL_PRED_AVERAGE,
  GEOZL_PRED_WP_STATIC,
  GEOZL_PRED_DELTA_1D,
  GEOZL_PRED_ID,
  GEOZL_PRED_COUNT
} geozl_predictor;

// A terminal selects the layout and backend.
typedef enum {
  GEOZL_TERM_ENTROPY = 0, // adaptive FSE/Huffman
  GEOZL_TERM_FIELD_LZ,    // field-wise LZ over the numeric residual
  GEOZL_TERM_ZSTD,        // serialize, then zstd
  GEOZL_TERM_CATEGORICAL, // dispatch on the dominant symbol's share
  GEOZL_TERM_T_ENTROPY,   // transpose to lanes, entropy (shuffle + entropy)
  GEOZL_TERM_T_ZSTD,      // transpose to lanes, zstd (shuffle + zstd)
  GEOZL_TERM_PFOR,        // bit pack fixed blocks and patch exceptions
  GEOZL_TERM_PIVCO,       // one byte lane, PivCo or PFOR
  GEOZL_TERM_T_PIVCO,     // transpose to lanes, PivCo or PFOR per lane
  GEOZL_TERM_SPARSE,      // sparse_num when one value dominates
  GEOZL_TERM_COUNT
} geozl_terminal;

// Dominant-symbol threshold measured on synthetic categorical fields.
#define GEOZL_CATEGORICAL_LZ 0.95

// Use sparse_num only when a value has a strict majority.
#define GEOZL_SPARSE_DOMINANT 0.5

static const char *pred_name(geozl_predictor p) {
  switch (p) {
  case GEOZL_PRED_DELTA_W:   return "delta_w";
  case GEOZL_PRED_DELTA_N:   return "delta_n";
  case GEOZL_PRED_PLANAR:    return "planar";
  case GEOZL_PRED_MED:       return "med";
  case GEOZL_PRED_AVERAGE:   return "average";
  case GEOZL_PRED_WP_STATIC: return "wp_static";
  case GEOZL_PRED_DELTA_1D:  return "delta_1d";
  case GEOZL_PRED_ID:        return "id";
  default:                   return "?";
  }
}

static const char *term_name(geozl_terminal t) {
  switch (t) {
  case GEOZL_TERM_ENTROPY:   return "entropy";
  case GEOZL_TERM_FIELD_LZ:  return "field_lz";
  case GEOZL_TERM_ZSTD:      return "zstd";
  case GEOZL_TERM_CATEGORICAL: return "categorical";
  case GEOZL_TERM_T_ENTROPY: return "transpose>entropy";
  case GEOZL_TERM_T_ZSTD:    return "transpose>zstd";
  case GEOZL_TERM_PFOR:      return "pfor";
  case GEOZL_TERM_PIVCO:     return "pivco";
  case GEOZL_TERM_T_PIVCO:   return "transpose>pivco";
  case GEOZL_TERM_SPARSE:    return "sparse";
  default:                   return "?";
  }
}

// Format a candidate recipe.
static void candidate_name(geozl_predictor p, geozl_terminal t, char *out,
                           size_t cap) {
  if (p == GEOZL_PRED_ID)
    snprintf(out, cap, "id>%s", term_name(t));
  else if (p == GEOZL_PRED_DELTA_1D)
    snprintf(out, cap, "delta_1d>%s", term_name(t));
  else
    snprintf(out, cap, "%s>zigzag>%s", pred_name(p), term_name(t));
}

// Match against candidate_name so parsing and formatting share one spelling.
static int parse_candidate(const char *name, geozl_predictor *outP,
                           geozl_terminal *outT) {
  if (name == NULL || name[0] == '\0')
    return -1;
  char buf[48];
  for (int p = 0; p < GEOZL_PRED_COUNT; ++p) {
    for (int t = 0; t < GEOZL_TERM_COUNT; ++t) {
      candidate_name((geozl_predictor)p, (geozl_terminal)t, buf, sizeof(buf));
      if (strcmp(name, buf) == 0) {
        *outP = (geozl_predictor)p;
        *outT = (geozl_terminal)t;
        return 0;
      }
    }
  }
  return -1;
}

static ZL_NodeID predictor_node(ZL_Compressor *c, geozl_predictor p,
                                uint32_t width, uint32_t planes) {
  switch (p) {
  case GEOZL_PRED_DELTA_W:   return geozl_node_delta_w(c, width);
  case GEOZL_PRED_DELTA_N:   return geozl_node_delta_n(c, width, planes);
  case GEOZL_PRED_PLANAR:    return geozl_node_planar(c, width, planes);
  case GEOZL_PRED_MED:       return geozl_node_med(c, width, planes);
  case GEOZL_PRED_AVERAGE:   return geozl_node_average(c, width, planes);
  case GEOZL_PRED_WP_STATIC: return geozl_node_wp_static(c, width, planes);
  default:                   return ZL_NODE_ILLEGAL;
  }
}

// n == 0 is the id branch, a backend with no predictor in front of it.
static ZL_GraphID chain(ZL_Compressor *c, const ZL_NodeID *nodes, size_t n,
                        ZL_GraphID final) {
  if (n == 0)
    return final;
  if (n == 1)
    return ZL_Compressor_registerStaticGraph_fromNode1o(c, nodes[0], final);
  return ZL_Compressor_registerStaticGraph_fromPipelineNodes1o(c, nodes, n,
                                                               final);
}

// Count the dominant value with Boyer-Moore followed by an exact pass.
static size_t dominant_count(const void *src, size_t n, size_t eltWidth) {
  const unsigned char *p = (const unsigned char *)src;
  uint64_t cand = 0;
  size_t votes = 0;
  for (size_t i = 0; i < n; ++i) {
    uint64_t v = 0;
    memcpy(&v, p + i * eltWidth, eltWidth);
    if (votes == 0) {
      cand = v;
      votes = 1;
    } else if (v == cand) {
      ++votes;
    } else {
      --votes;
    }
  }
  size_t hits = 0;
  for (size_t i = 0; i < n; ++i) {
    uint64_t v = 0;
    memcpy(&v, p + i * eltWidth, eltWidth);
    hits += (v == cand);
  }
  return hits;
}

// Select constant, field-LZ or entropy by the dominant symbol's share.
static ZL_Report geozl_categorical_fg(ZL_Graph *g, ZL_Edge *inputs[],
                                      size_t nbInputs) {
  (void)g;
  (void)nbInputs;
  const ZL_Input *in = ZL_Edge_getData(inputs[0]);
  const size_t n = ZL_Input_numElts(in);
  // ZL_GRAPH_CONSTANT refuses an empty stream, and an empty one has no dominant
  // symbol to measure.
  if (n == 0)
    return ZL_Edge_setDestination(inputs[0], ZL_GRAPH_ENTROPY);

  const size_t hits =
      dominant_count(ZL_Input_ptr(in), n, ZL_Input_eltWidth(in));
  ZL_GraphID dst = ZL_GRAPH_ENTROPY;
  if (hits == n)
    dst = ZL_GRAPH_CONSTANT;
  else if ((double)hits > GEOZL_CATEGORICAL_LZ * (double)n)
    dst = ZL_GRAPH_FIELD_LZ;
  return ZL_Edge_setDestination(inputs[0], dst);
}

// The same count for byte streams, which are all PivCo is offered. Four
// histograms so consecutive equal bytes do not wait on each other's increment.
static size_t dominant_count_u8(const unsigned char *p, size_t n) {
  size_t h[4][256] = {{0}};
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    h[0][p[i]]++;
    h[1][p[i + 1]]++;
    h[2][p[i + 2]]++;
    h[3][p[i + 3]]++;
  }
  for (; i < n; ++i)
    h[0][p[i]]++;
  size_t hits = 0;
  for (int s = 0; s < 256; ++s) {
    const size_t c = h[0][s] + h[1][s] + h[2][s] + h[3][s];
    hits = c > hits ? c : hits;
  }
  return hits;
}

static ZL_GraphID transpose_pivco_graph(ZL_Compressor *c) {
  const ZL_NodeID node = geozl_node_transpose_pivco(c);
  if (!ZL_NodeID_isValid(node))
    return ZL_GRAPH_ILLEGAL;
  return ZL_Compressor_registerStaticGraph_fromNode(
      c, node, ZL_GRAPHLIST(ZL_GRAPH_ENTROPY, ZL_GRAPH_STORE));
}

// Select sparse_num for dominated streams.
static ZL_Report geozl_sparse_fg(ZL_Graph *g, ZL_Edge *inputs[],
                                 size_t nbInputs) {
  (void)nbInputs;
  const ZL_Input *in = ZL_Edge_getData(inputs[0]);
  const size_t n = ZL_Input_numElts(in);
  const size_t w = ZL_Input_eltWidth(in);
  const size_t hits = w == 1 ? dominant_count_u8(ZL_Input_ptr(in), n)
                             : dominant_count(ZL_Input_ptr(in), n, w);
  ZL_GraphID dst = w <= 2 ? ZL_GRAPH_ENTROPY : ZL_GRAPH_NUMERIC;
  if ((double)hits > GEOZL_SPARSE_DOMINANT * (double)n)
    dst = ZL_Graph_getCustomGraphs(g).graphids[0];
  return ZL_Edge_setDestination(inputs[0], dst);
}

// One candidate graph, or ZL_GRAPH_ILLEGAL if the pair does not apply.
static ZL_GraphID build_candidate(ZL_Compressor *c, geozl_predictor p,
                                  geozl_terminal t, uint32_t width,
                                  uint32_t planes, size_t eltWidth) {
  if (p == GEOZL_PRED_PLANAR && t == GEOZL_TERM_PFOR) {
    ZL_NodeID fused = geozl_node_planar_zigzag_pfor(c, width, planes);
    if (!ZL_NodeID_isValid(fused))
      return ZL_GRAPH_ILLEGAL;
    return ZL_Compressor_registerStaticGraph_fromNode1o(c, fused,
                                                         ZL_GRAPH_STORE);
  }
  if (p == GEOZL_PRED_PLANAR &&
      (t == GEOZL_TERM_T_PIVCO || (t == GEOZL_TERM_PIVCO && eltWidth == 1))) {
    ZL_NodeID fused =
        geozl_node_planar_zigzag_transpose_pivco(c, width, planes);
    if (!ZL_NodeID_isValid(fused))
      return ZL_GRAPH_ILLEGAL;
    return ZL_Compressor_registerStaticGraph_fromNode(
        c, fused, ZL_GRAPHLIST(ZL_GRAPH_ENTROPY, ZL_GRAPH_STORE));
  }

  ZL_NodeID head[3];
  size_t n = 0;
  if (p == GEOZL_PRED_ID) {
    // no residual: the raw numeric stream feeds the terminal
  } else if (p == GEOZL_PRED_DELTA_1D) {
    head[n++] = ZL_NODE_DELTA_INT;
  } else if (p == GEOZL_PRED_PLANAR || p == GEOZL_PRED_MED) {
    ZL_NodeID fused = (p == GEOZL_PRED_PLANAR)
                          ? geozl_node_planar_zigzag(c, width, planes)
                          : geozl_node_med_zigzag(c, width, planes);
    if (!ZL_NodeID_isValid(fused))
      return ZL_GRAPH_ILLEGAL;
    head[n++] = fused;
  } else {
    ZL_NodeID pred = predictor_node(c, p, width, planes);
    if (!ZL_NodeID_isValid(pred))
      return ZL_GRAPH_ILLEGAL;
    head[n++] = pred;
    head[n++] = ZL_NODE_ZIGZAG;
  }

  switch (t) {
  case GEOZL_TERM_ENTROPY:
    return chain(c, head, n, ZL_GRAPH_ENTROPY);
  case GEOZL_TERM_FIELD_LZ:
    return chain(c, head, n, ZL_GRAPH_FIELD_LZ);
  case GEOZL_TERM_ZSTD:
    head[n++] = ZL_NODE_CONVERT_NUM_TO_SERIAL;
    return chain(c, head, n, ZL_GRAPH_ZSTD);
  case GEOZL_TERM_CATEGORICAL: {
    // The entropy arm takes 1 or 2 bytes and the other two take any width, so
    // without this the terminal would apply or not by what the tile happens to
    // hold. Numeric in, no transpose, so no conversion node ahead of it.
    if (eltWidth > 2)
      return ZL_GRAPH_ILLEGAL;
    static const ZL_Type in_mask = ZL_Type_numeric;
    static const ZL_GraphID used[3] = {ZL_GRAPH_CONSTANT, ZL_GRAPH_FIELD_LZ,
                                       ZL_GRAPH_ENTROPY};
    ZL_FunctionGraphDesc desc = {0};
    desc.name = "geozl_categorical";
    desc.graph_f = geozl_categorical_fg;
    desc.inputTypeMasks = &in_mask;
    desc.nbInputs = 1;
    desc.customGraphs = used;
    desc.nbCustomGraphs = 3;
    ZL_GraphID fg = ZL_Compressor_registerFunctionGraph(c, &desc);
    if (!ZL_GraphID_isValid(fg))
      return ZL_GRAPH_ILLEGAL;
    return chain(c, head, n, fg);
  }
  case GEOZL_TERM_PIVCO: {
    // A bare PivCo terminal is one byte lane. Wider elements spell out the
    // transpose, which selects the same lane codec with a wider header.
    if (eltWidth != 1)
      return ZL_GRAPH_ILLEGAL;
    ZL_GraphID pg = transpose_pivco_graph(c);
    if (!ZL_GraphID_isValid(pg))
      return ZL_GRAPH_ILLEGAL;
    return chain(c, head, n, pg);
  }
  case GEOZL_TERM_SPARSE: {
    // Run lengths and literals use OpenZL's numeric graph.
    static const ZL_Type in_mask = ZL_Type_numeric;
    const ZL_GraphID used[3] = {
        ZL_Compressor_registerStaticGraph_fromNode(
            c, ZL_NODE_SPARSE_NUM_AUTO,
            ZL_GRAPHLIST(ZL_GRAPH_NUMERIC, ZL_GRAPH_NUMERIC)),
        ZL_GRAPH_ENTROPY, ZL_GRAPH_NUMERIC};
    if (!ZL_GraphID_isValid(used[0]))
      return ZL_GRAPH_ILLEGAL;
    ZL_FunctionGraphDesc desc = {0};
    desc.name = "geozl_sparse";
    desc.graph_f = geozl_sparse_fg;
    desc.inputTypeMasks = &in_mask;
    desc.nbInputs = 1;
    desc.customGraphs = used;
    desc.nbCustomGraphs = 3;
    ZL_GraphID sg = ZL_Compressor_registerFunctionGraph(c, &desc);
    if (!ZL_GraphID_isValid(sg))
      return ZL_GRAPH_ILLEGAL;
    return chain(c, head, n, sg);
  }
  case GEOZL_TERM_T_ENTROPY:
  case GEOZL_TERM_T_ZSTD: {
    // transpose to byte lanes, every lane to one backend
    if (eltWidth < 2 || eltWidth > 8)
      return ZL_GRAPH_ILLEGAL;
    ZL_GraphID back = t == GEOZL_TERM_T_ZSTD ? ZL_GRAPH_ZSTD : ZL_GRAPH_ENTROPY;
    if (!ZL_GraphID_isValid(back))
      return ZL_GRAPH_ILLEGAL;
    ZL_GraphID tg = ZL_Compressor_registerTransposeSplitGraph(c, back);
    if (!ZL_GraphID_isValid(tg))
      return ZL_GRAPH_ILLEGAL;
    head[n++] = ZL_NODE_CONVERT_NUM_TO_STRUCT_LE;
    return chain(c, head, n, tg);
  }
  case GEOZL_TERM_T_PIVCO: {
    if (eltWidth != 2 && eltWidth != 4 && eltWidth != 8)
      return ZL_GRAPH_ILLEGAL;
    ZL_GraphID pg = transpose_pivco_graph(c);
    if (!ZL_GraphID_isValid(pg))
      return ZL_GRAPH_ILLEGAL;
    return chain(c, head, n, pg);
  }
  case GEOZL_TERM_PFOR: {
    ZL_NodeID nd = geozl_node_pfor(c);
    if (!ZL_NodeID_isValid(nd))
      return ZL_GRAPH_ILLEGAL;
    head[n++] = nd;
    return chain(c, head, n, ZL_GRAPH_STORE);
  }
  default:
    return ZL_GRAPH_ILLEGAL;
  }
}

// Add optional quantizer and nodata stages around a candidate.
static ZL_GraphID build_graph(ZL_Compressor *c, geozl_predictor p,
                                 geozl_terminal t, uint32_t width,
                                 uint32_t planes, size_t eltWidth,
                                 const geozl_lossy_plan *plan, int dtype,
                                 int nodataMode, uint64_t nodataBits,
                                 double guardRadius) {
  ZL_GraphID sel = build_candidate(c, p, t, width, planes, eltWidth);
  if (!ZL_GraphID_isValid(sel))
    return ZL_GRAPH_ILLEGAL;

  if (plan->family != GEOZL_LOSSY_NONE) {
    ZL_NodeID q = geozl_node_lossy(c, plan, dtype);
    if (!ZL_NodeID_isValid(q))
      return ZL_GRAPH_ILLEGAL;
    sel = ZL_Compressor_registerStaticGraph_fromNode1o(c, q, sel);
    if (!ZL_GraphID_isValid(sel))
      return ZL_GRAPH_ILLEGAL;
  }

  if (nodataMode == GEOZL_NODATA_NONE)
    return sel;

  // The sentinel arrives as 64 bits whatever the sample width, so cut it down
  // to the bits an element actually holds before it reaches the header.
  uint64_t bits = 0;
  if (nodataMode == GEOZL_NODATA_VALUE)
    bits = (eltWidth == 8)
               ? nodataBits
               : (nodataBits & (((uint64_t)1 << (8 * eltWidth)) - 1));

  ZL_NodeID nd = geozl_node_nodata(c, width, (geozl_nodata_mode)nodataMode,
                                   bits, dtype, guardRadius);
  if (!ZL_NodeID_isValid(nd))
    return ZL_GRAPH_ILLEGAL;
  // Outcome 0 is the raster and carries on down the recipe, outcome 1 is the
  // mask.
  return ZL_Compressor_registerStaticGraph_fromNode(
      c, nd, ZL_GRAPHLIST(sel, ZL_GRAPH_COMPRESS_GENERIC));
}

// OpenZL error strings must be copied before their owner is freed.
typedef enum { ERR_NONE, ERR_COMPRESSOR, ERR_CCTX, ERR_DCTX } err_owner;

static void copy_err(char *dst, size_t cap, const char *src) {
  if (dst == NULL || cap == 0)
    return;
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// Reusable compressor and its fixed sample metadata.
struct geozl_2d_graph_s {
  ZL_Compressor *c;
  ZL_CCtx *cctx;
  geozl_lossy_plan plan;
  size_t eltWidth;
  int dtype;
  int nodataMode;
  uint64_t nodataBits;
};

GEOZL_API void geozl_2d_graph_close_c(geozl_2d_graph *g) {
  if (g == NULL)
    return;
  if (g->cctx != NULL)
    ZL_CCtx_free(g->cctx);
  if (g->c != NULL)
    ZL_Compressor_free(g->c);
  free(g);
}

// Read the sentinel without implementation-defined unsigned-to-signed casts.
static double sentinel_value(int dtype, uint64_t bits) {
  const size_t w = geozl_dtype_width(dtype);
  const uint64_t sign = (uint64_t)1 << (8 * w - 1);
  switch (dtype) {
  case GEOZL_DT_I8:
  case GEOZL_DT_I16:
  case GEOZL_DT_I32:
  case GEOZL_DT_I64:
    return (double)(bits & (sign - 1)) - ((bits & sign) ? (double)sign : 0.0);
  case GEOZL_DT_F16:
    return (double)geozl_half_to_float((uint16_t)bits);
  case GEOZL_DT_F32: {
    const uint32_t u = (uint32_t)bits;
    float f;
    memcpy(&f, &u, sizeof f);
    return (double)f;
  }
  case GEOZL_DT_F64: {
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
  }
  default:
    return (double)bits;
  }
}

// Build a graph and bind its context. src resolves lossy parameters. On failure,
// *out is NULL. checksum == 0 disables both frame checksums.
static ZL_Report graph_open(geozl_2d_graph **out, const char *method,
                            uint32_t width, uint32_t planes,
                            const char *error, int dtype,
                            int nodataMode, uint64_t nodataBits,
                            const void *src, size_t numElts, size_t eltWidth,
                            int checksum, char *errCtx, size_t errCtxSize) {
  const int has_err = errCtx != NULL && errCtxSize != 0;
  geozl_2d_graph *e;
  *out = NULL;

  // ZL_TypedRef_createNumeric only returns NULL, so an unsupported width would
  // otherwise be reported as an allocation failure.
  if (eltWidth != 1 && eltWidth != 2 && eltWidth != 4 && eltWidth != 8) {
    if (has_err)
      snprintf(errCtx, errCtxSize,
               "eltWidth %zu, an OpenZL numeric stream is 1, 2, 4 or 8",
               eltWidth);
    return ZL_returnError(ZL_ErrorCode_parameter_invalid);
  }

  // Every quantizer indexes tables by this, checked once here.
  if (!GEOZL_DT_OK(dtype)) {
    if (has_err)
      snprintf(errCtx, errCtxSize,
               "dtype %d is outside the geozl_dtype codes, which run 0 to %d",
               dtype, (int)GEOZL_DT_F64);
    return ZL_returnError(ZL_ErrorCode_parameter_invalid);
  }

  // Report unknown names separately from recipes unsupported at this width.
  geozl_predictor pred;
  geozl_terminal term;
  if (parse_candidate(method, &pred, &term) != 0) {
    if (has_err)
      snprintf(errCtx, errCtxSize, "unknown method \"%s\"",
               method != NULL ? method : "(null)");
    return ZL_returnError(ZL_ErrorCode_parameter_invalid);
  }

  // Resolve once so compress and benchmark use the same parameters.
  geozl_lossy_recipe recipe;
  geozl_lossy_plan plan;
  {
    char why[192] = {0};
    if (geozl_lossy_parse(error, &recipe, why, sizeof(why)) != 0 ||
        geozl_lossy_fit(&recipe, src, dtype, width, numElts, why,
                        sizeof(why)) != 0 ||
        geozl_lossy_resolve(&recipe, src, dtype, numElts, &plan, why,
                            sizeof(why)) != 0) {
      if (has_err)
        snprintf(errCtx, errCtxSize, "%s", why);
      return ZL_returnError(ZL_ErrorCode_parameter_invalid);
    }
  }

  e = calloc(1, sizeof(*e));
  if (e == NULL)
    return ZL_returnError(ZL_ErrorCode_allocation);
  e->plan = plan;
  e->eltWidth = eltWidth;
  e->dtype = dtype;
  e->nodataMode = nodataMode;
  e->nodataBits = nodataBits;

  e->c = ZL_Compressor_create();
  if (e->c == NULL) {
    free(e);
    return ZL_returnError(ZL_ErrorCode_allocation);
  }

  ZL_Report r;
  err_owner owner = ERR_NONE;

  const uint64_t sentinelBits =
      (eltWidth == 8) ? nodataBits
                      : (nodataBits & (((uint64_t)1 << (8 * eltWidth)) - 1));
  // A NaN hole has no neighbourhood for a bound to reach, so its radius only
  // says whether the path is lossless.
  const double radius =
      nodataMode == GEOZL_NODATA_NAN
          ? (recipe.family == GEOZL_LOSSY_NONE ? 0.0 : GEOZL_F64_INF)
          : geozl_lossy_guard_radius(&recipe,
                                     sentinel_value(dtype, sentinelBits));
  ZL_GraphID g = build_graph(e->c, pred, term, width, planes, eltWidth, &plan, dtype,
                             nodataMode, nodataBits, radius);
  if (!ZL_GraphID_isValid(g)) {
    if (has_err)
      snprintf(errCtx, errCtxSize,
               "method \"%s\" does not apply to %zu-byte elements; the "
               "transpose terminals need 2 to 8, categorical needs 1 or 2, "
               "and pivco without transpose needs 1",
               method, eltWidth);
    r = ZL_returnError(ZL_ErrorCode_graph_invalid);
    goto fail;
  }
  r = ZL_Compressor_selectStartingGraphID(e->c, g);
  if (ZL_isError(r)) {
    owner = ERR_COMPRESSOR;
    goto fail;
  }

  e->cctx = ZL_CCtx_create();
  if (e->cctx == NULL) {
    r = ZL_returnError(ZL_ErrorCode_allocation);
    goto fail;
  }
  // refCompressor resets any parameter already set, so it goes first.
  r = ZL_CCtx_refCompressor(e->cctx, e->c);
  if (ZL_isError(r)) {
    owner = ERR_CCTX;
    goto fail;
  }
  // OpenZL clears CCtx parameters after each session, and the bench runs many.
  r = ZL_CCtx_setParameter(e->cctx, ZL_CParam_stickyParameters, 1);
  if (ZL_isError(r)) {
    owner = ERR_CCTX;
    goto fail;
  }
  const int openzl03 = term == GEOZL_TERM_PIVCO ||
                       term == GEOZL_TERM_T_PIVCO || term == GEOZL_TERM_SPARSE;
  r = ZL_CCtx_setParameter(e->cctx, ZL_CParam_formatVersion,
                           openzl03 ? GEOZL_OPENZL03_FORMAT_VERSION
                                    : GEOZL_FORMAT_VERSION);
  if (ZL_isError(r)) {
    owner = ERR_CCTX;
    goto fail;
  }
  if (!checksum || plan.family != GEOZL_LOSSY_NONE) {
    r = ZL_CCtx_setParameter(e->cctx, ZL_CParam_contentChecksum,
                             ZL_TernaryParam_disable);
    if (ZL_isError(r)) {
      owner = ERR_CCTX;
      goto fail;
    }
  }
  if (!checksum) {
    r = ZL_CCtx_setParameter(e->cctx, ZL_CParam_compressedChecksum,
                             ZL_TernaryParam_disable);
    if (ZL_isError(r)) {
      owner = ERR_CCTX;
      goto fail;
    }
  }
  *out = e;
  return ZL_returnSuccess();

fail:
  if (owner == ERR_CCTX)
    copy_err(errCtx, errCtxSize, ZL_CCtx_getErrorContextString(e->cctx, r));
  else if (owner == ERR_COMPRESSOR)
    copy_err(errCtx, errCtxSize, ZL_Compressor_getErrorContextString(e->c, r));
  geozl_2d_graph_close_c(e);
  return r;
}

// Compress once with a prepared graph.
static ZL_Report graph_run(const geozl_2d_graph *e, const void *src,
                           size_t numElts, const void *coeffs,
                           size_t coeffsSize, void *dst, size_t dstCapacity,
                           size_t *outSize, char *errCtx, size_t errCtxSize) {
  if (e->plan.family != GEOZL_LOSSY_NONE) {
    char why[192] = {0};
    const int ignoreValue = e->nodataMode == GEOZL_NODATA_VALUE;
    if (geozl_lossy_check_domain(&e->plan, src, e->dtype, numElts,
                                 ignoreValue, e->nodataBits, why,
                                 sizeof(why)) != 0) {
      copy_err(errCtx, errCtxSize, why);
      return ZL_returnError(ZL_ErrorCode_parameter_invalid);
    }
  }
  if ((coeffs == NULL) != (coeffsSize == 0) ||
      (coeffs != NULL &&
       geozl_coeffs_parse(coeffs, coeffsSize, NULL, 0, NULL, 0, NULL, NULL) !=
           0)) {
    copy_err(errCtx, errCtxSize, "coefficients are not a valid blob");
    return ZL_returnError(ZL_ErrorCode_parameter_invalid);
  }
  ZL_TypedRef *in = ZL_TypedRef_createNumeric(src, e->eltWidth, numElts);
  if (in == NULL) {
    if (errCtx != NULL && errCtxSize != 0)
      snprintf(errCtx, errCtxSize, "could not wrap src as a numeric stream");
    return ZL_returnError(ZL_ErrorCode_allocation);
  }
  // OpenZL clears the comment when compression finishes. Keep the add and
  // compress calls adjacent so an early return cannot retain it.
  if (coeffs != NULL) {
    ZL_Report cr = ZL_CCtx_addHeaderComment(e->cctx, coeffs, coeffsSize);
    if (ZL_isError(cr)) {
      copy_err(errCtx, errCtxSize, ZL_CCtx_getErrorContextString(e->cctx, cr));
      ZL_TypedRef_free(in);
      return cr;
    }
  }
  ZL_Report r = ZL_CCtx_compressTypedRef(e->cctx, dst, dstCapacity, in);
  ZL_TypedRef_free(in);
  if (ZL_isError(r))
    copy_err(errCtx, errCtxSize, ZL_CCtx_getErrorContextString(e->cctx, r));
  else if (outSize != NULL)
    *outSize = ZL_validResult(r);
  return r;
}

GEOZL_API int geozl_2d_graph_open_c(geozl_2d_graph **out, const char *method,
                                    uint32_t width, uint32_t planes,
                                    const char *error,
                                    int dtype, int nodataMode,
                                    uint64_t nodataBits, const void *src,
                                    size_t numElts, size_t eltWidth,
                                    char *errCtx, size_t errCtxSize) {
  if (errCtx != NULL && errCtxSize != 0)
    errCtx[0] = '\0';
  ZL_Report r =
      graph_open(out, method, width, planes, error, dtype, nodataMode, nodataBits, src,
                 numElts, eltWidth, 1, errCtx, errCtxSize);
  return ZL_isError(r) ? (int)ZL_errorCode(r) : 0;
}

GEOZL_API int geozl_2d_compress_graph_c(geozl_2d_graph *g, const void *src,
                                        size_t numElts, void *dst,
                                        size_t dstCapacity, size_t *outSize,
                                        char *errCtx, size_t errCtxSize) {
  if (errCtx != NULL && errCtxSize != 0)
    errCtx[0] = '\0';
  ZL_Report r = graph_run(g, src, numElts, NULL, 0, dst, dstCapacity, outSize,
                          errCtx, errCtxSize);
  return ZL_isError(r) ? (int)ZL_errorCode(r) : 0;
}

GEOZL_API int geozl_2d_compress_coeffs_c(geozl_2d_graph *g, const void *src,
                                         size_t numElts, const void *coeffs,
                                         size_t coeffsSize, void *dst,
                                         size_t dstCapacity, size_t *outSize,
                                         char *errCtx, size_t errCtxSize) {
  if (errCtx != NULL && errCtxSize != 0)
    errCtx[0] = '\0';
  ZL_Report r = graph_run(g, src, numElts, coeffs, coeffsSize, dst, dstCapacity,
                          outSize, errCtx, errCtxSize);
  return ZL_isError(r) ? (int)ZL_errorCode(r) : 0;
}

GEOZL_API int geozl_2d_frame_coeffs_c(const void *frame, size_t frameSize,
                                      void *dst, size_t dstCapacity,
                                      size_t *outSize) {
  if (outSize != NULL)
    *outSize = 0;
  ZL_FrameInfo *fi = ZL_FrameInfo_create(frame, frameSize);
  if (fi == NULL)
    return (int)ZL_ErrorCode_corruption;

  ZL_RESULT_OF(ZL_Comment) cr = ZL_FrameInfo_getComment(fi);
  int rc;
  if (ZL_RES_isError(cr)) {
    ZL_Report v = ZL_FrameInfo_getFormatVersion(fi);
    rc = (!ZL_isError(v) &&
          ZL_validResult(v) < OPENZL_COMMENT_VERSION_MIN)
             ? -1
             : (int)ZL_RES_code(cr);
  } else {
    ZL_Comment cm = ZL_RES_value(cr);
    int parsed = cm.size == 0
                     ? 1
                     : geozl_coeffs_parse(cm.data, cm.size, NULL, 0, NULL, 0,
                                          NULL, NULL);
    if (parsed == 1) {
      rc = -1;
    } else if (parsed != 0) {
      rc = (int)ZL_ErrorCode_corruption;
    } else if (outSize != NULL && dst == NULL) {
      *outSize = cm.size;
      rc = 0;
    } else if (dst == NULL || dstCapacity < cm.size) {
      rc = (int)ZL_ErrorCode_dstCapacity_tooSmall;
    } else {
      memcpy(dst, cm.data, cm.size);
      if (outSize != NULL)
        *outSize = cm.size;
      rc = 0;
    }
  }
  ZL_FrameInfo_free(fi);
  return rc;
}

GEOZL_API int geozl_2d_compress_c(const char *method, uint32_t width,
                                  uint32_t planes,
                                  const char *error, int dtype, int nodataMode,
                                  uint64_t nodataBits, const void *src,
                                  size_t numElts, size_t eltWidth, void *dst,
                                  size_t dstCapacity, size_t *outSize,
                                  char *errCtx, size_t errCtxSize) {
  if (errCtx != NULL && errCtxSize != 0)
    errCtx[0] = '\0';

  geozl_2d_graph *e;
  ZL_Report r = graph_open(&e, method, width, planes, error, dtype, nodataMode,
                           nodataBits, src, numElts, eltWidth, 1, errCtx,
                           errCtxSize);
  if (!ZL_isError(r)) {
    r = graph_run(e, src, numElts, NULL, 0, dst, dstCapacity, outSize, errCtx,
                  errCtxSize);
    geozl_2d_graph_close_c(e);
  }
  return ZL_isError(r) ? (int)ZL_errorCode(r) : 0;
}

GEOZL_API size_t geozl_2d_frame_dsize_c(const void *frame, size_t frameSize) {
  ZL_Report r = ZL_getDecompressedSize(frame, frameSize);
  return ZL_isError(r) ? 0 : ZL_validResult(r);
}

typedef struct {
  ZL_DCtx *dctx;
} geozl_decoder;

static void decoder_close(geozl_decoder *d) {
  if (d->dctx != NULL)
    ZL_DCtx_free(d->dctx);
  d->dctx = NULL;
}

// verify == 0 skips checksum verification.
static ZL_Report decoder_open(geozl_decoder *d, int verify, char *errCtx,
                              size_t errCtxSize) {
  d->dctx = ZL_DCtx_create();
  if (d->dctx == NULL)
    return ZL_returnError(ZL_ErrorCode_allocation);

  ZL_Report r = geozl_register_decoders(d->dctx);
  if (ZL_isError(r))
    goto fail;
  if (!verify) {
    // Same session reset as the CCtx above.
    r = ZL_DCtx_setParameter(d->dctx, ZL_DParam_stickyParameters, 1);
    if (ZL_isError(r))
      goto fail;
    r = ZL_DCtx_setParameter(d->dctx, ZL_DParam_checkCompressedChecksum,
                             ZL_TernaryParam_disable);
    if (ZL_isError(r))
      goto fail;
    r = ZL_DCtx_setParameter(d->dctx, ZL_DParam_checkContentChecksum,
                             ZL_TernaryParam_disable);
    if (ZL_isError(r))
      goto fail;
  }
  return ZL_returnSuccess();

fail:
  copy_err(errCtx, errCtxSize, ZL_DCtx_getErrorContextString(d->dctx, r));
  decoder_close(d);
  return r;
}

static ZL_Report decoder_run(const geozl_decoder *d, const void *frame,
                             size_t frameSize, void *dst, size_t dstCapacity,
                             size_t *outSize, char *errCtx, size_t errCtxSize) {
  // Our frames carry a single numeric output; ZL_DCtx_decompress only returns
  // serial, so the typed variant is required. dst must be 8-byte aligned.
  ZL_OutputInfo info;
  ZL_Report r =
      ZL_DCtx_decompressTyped(d->dctx, &info, dst, dstCapacity, frame,
                              frameSize);
  if (ZL_isError(r))
    copy_err(errCtx, errCtxSize, ZL_DCtx_getErrorContextString(d->dctx, r));
  else if (outSize != NULL)
    *outSize = (size_t)info.decompressedByteSize;
  return r;
}

GEOZL_API int geozl_2d_decompress_c(const void *frame, size_t frameSize,
                                    void *dst, size_t dstCapacity,
                                    size_t *outSize, int verify, char *errCtx,
                                    size_t errCtxSize) {
  if (errCtx != NULL && errCtxSize != 0)
    errCtx[0] = '\0';

  geozl_decoder d;
  ZL_Report r = decoder_open(&d, verify, errCtx, errCtxSize);
  if (!ZL_isError(r)) {
    r = decoder_run(&d, frame, frameSize, dst, dstCapacity, outSize, errCtx,
                    errCtxSize);
    decoder_close(&d);
  }
  return ZL_isError(r) ? (int)ZL_errorCode(r) : 0;
}

#ifdef _WIN32
// The Windows C runtime has no clock_gettime.
static double now_sec(void) {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)f.QuadPart;
}
#else
static double now_sec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
#endif

// Fall back to the run mean when one repetition is below clock resolution.
static double best_or_mean(double best, double run, size_t reps) {
  return best > 0.0 ? best : run / (double)reps;
}

GEOZL_API int geozl_2d_bench_c(const char *method, uint32_t width,
                               uint32_t planes,
                               const char *error, int dtype, int nodataMode,
                               uint64_t nodataBits, const void *src,
                               size_t numElts, size_t eltWidth, size_t reps,
                               int checksum, int verify, size_t *compSize,
                               double *encSec, double *decSec, char *errCtx,
                               size_t errCtxSize) {
  const int has_err = errCtx != NULL && errCtxSize != 0;
  if (has_err)
    errCtx[0] = '\0';

  if (reps == 0) {
    if (has_err)
      snprintf(errCtx, errCtxSize, "reps is 0, there is nothing to time");
    return (int)ZL_ErrorCode_parameter_invalid;
  }

  geozl_2d_graph *enc;
  ZL_Report r = graph_open(&enc, method, width, planes, error, dtype, nodataMode,
                           nodataBits, src, numElts, eltWidth, checksum,
                           errCtx, errCtxSize);
  if (ZL_isError(r))
    return (int)ZL_errorCode(r);

  geozl_decoder dec;
  r = decoder_open(&dec, verify, errCtx, errCtxSize);
  if (ZL_isError(r)) {
    geozl_2d_graph_close_c(enc);
    return (int)ZL_errorCode(r);
  }

  size_t cap = 1024 + numElts * eltWidth + numElts * eltWidth / 2;
  void *cbuf = malloc(cap);
  void *dbuf = NULL;
  double bestEnc = DBL_MAX;
  double bestDec = DBL_MAX;
  double runEnc = 0.0;
  double runDec = 0.0;
  double mark;
  size_t sz = 0;
  size_t dsize;
  int rc = 0;

  if (cbuf == NULL) {
    rc = (int)ZL_ErrorCode_allocation;
    goto done;
  }

  mark = now_sec();
  for (size_t i = 0; i < reps; ++i) {
    double t0 = now_sec();
    r = graph_run(enc, src, numElts, NULL, 0, cbuf, cap, &sz, errCtx,
                  errCtxSize);
    double dt = now_sec() - t0;
    if (ZL_isError(r)) {
      rc = (int)ZL_errorCode(r);
      goto done;
    }
    if (dt < bestEnc)
      bestEnc = dt;
  }
  runEnc = now_sec() - mark;

  dsize = geozl_2d_frame_dsize_c(cbuf, sz);
  dbuf = malloc(dsize ? dsize : 1); // malloc is max-aligned, fine for numeric
  if (dbuf == NULL) {
    rc = (int)ZL_ErrorCode_allocation;
    goto done;
  }

  mark = now_sec();
  for (size_t i = 0; i < reps; ++i) {
    double t0 = now_sec();
    r = decoder_run(&dec, cbuf, sz, dbuf, dsize, NULL, errCtx, errCtxSize);
    double dt = now_sec() - t0;
    if (ZL_isError(r)) {
      rc = (int)ZL_errorCode(r);
      goto done;
    }
    if (dt < bestDec)
      bestDec = dt;
  }
  runDec = now_sec() - mark;

  if (compSize != NULL)
    *compSize = sz;
  if (encSec != NULL)
    *encSec = best_or_mean(bestEnc, runEnc, reps);
  if (decSec != NULL)
    *decSec = best_or_mean(bestDec, runDec, reps);

done:
  free(dbuf);
  free(cbuf);
  decoder_close(&dec);
  geozl_2d_graph_close_c(enc);
  return rc;
}

// Resolve a profiler prior into predictors. A named predictor includes ID.
static int resolve_prior(const char *method, geozl_predictor *out,
                         size_t *outN) {
  if (method == NULL || method[0] == '\0') {
    size_t k = 0;
    for (int p = 0; p < GEOZL_PRED_COUNT; ++p)
      out[k++] = (geozl_predictor)p; // includes GEOZL_PRED_ID last
    *outN = k;
    return 0;
  }
  if (strcmp(method, "none") == 0 || strcmp(method, "id") == 0) {
    out[0] = GEOZL_PRED_ID;
    *outN = 1;
    return 0;
  }
  // Match pred_name so the spelling has one source.
  for (int p = 0; p < GEOZL_PRED_COUNT; ++p) {
    if (p == GEOZL_PRED_ID)
      continue; // handled above, and it takes no second pass
    if (strcmp(method, pred_name((geozl_predictor)p)) == 0) {
      out[0] = (geozl_predictor)p;
      out[1] = GEOZL_PRED_ID;
      *outN = 2;
      return 0;
    }
  }
  return -1;
}

GEOZL_API int geozl_2d_grid_c(const char *method, size_t eltWidth, char *names,
                              size_t stride, size_t maxNames, size_t *outCount) {
  geozl_predictor preds[GEOZL_PRED_COUNT];
  size_t nbPreds = 0;
  if (resolve_prior(method, preds, &nbPreds) != 0)
    return -1;

  size_t k = 0;
  for (size_t i = 0; i < nbPreds; ++i) {
    for (int t = 0; t < GEOZL_TERM_COUNT; ++t) {
      if (((geozl_terminal)t == GEOZL_TERM_T_ENTROPY ||
           (geozl_terminal)t == GEOZL_TERM_T_ZSTD ||
           (geozl_terminal)t == GEOZL_TERM_T_PIVCO) &&
          (eltWidth < 2 || eltWidth > 8))
        continue;
      // categorical's entropy arm tops out at 2, the same place ZL_GRAPH_ENTROPY
      // does
      if ((geozl_terminal)t == GEOZL_TERM_CATEGORICAL && eltWidth > 2)
        continue;
      // Plain pivco is one byte. Wider streams spell out the transpose.
      if ((geozl_terminal)t == GEOZL_TERM_PIVCO && eltWidth != 1)
        continue;
      if (k < maxNames)
        candidate_name(preds[i], (geozl_terminal)t, names + k * stride, stride);
      k++;
    }
  }
  if (outCount != NULL)
    *outCount = k;
  return 0;
}
