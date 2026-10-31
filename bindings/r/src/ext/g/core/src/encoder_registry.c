#include "geozl/geozl.h"

#include "average/encode_average_binding.h"
#include "deinterleave/encode_deinterleave_binding.h"
#include "delta_n/encode_delta_n_binding.h"
#include "delta_w/encode_delta_w_binding.h"
#include "med/encode_med_binding.h"
#include "med_zigzag/encode_med_zigzag_binding.h"
#include "nodata/encode_nodata_binding.h"
#include "pfor/encode_pfor_binding.h"
#include "planar/encode_planar_binding.h"
#include "planar_zigzag/encode_planar_zigzag_binding.h"
#include "planar_zigzag_pfor/encode_planar_zigzag_pfor_binding.h"
#include "planar_zigzag_transpose_pivco/encode_planar_zigzag_transpose_pivco_binding.h"
#include "quant_linear/encode_quant_linear_binding.h"
#include "quant_log/encode_quant_log_binding.h"
#include "quant_sqrt/encode_quant_sqrt_binding.h"
#include "transpose_pivco/encode_transpose_pivco_binding.h"
#include "wp_static/encode_wp_static_binding.h"

#include "common/endian.h"                   // geozl_st_le64
#include "common/graph_num1to1.h"            // GEOZL_PARAM_WIDTH
#include "lossy/lossy_node.h"                // geozl_lossy_plan
#include "quant_log/graph_quant_log.h"   // QUANT_LOG_PARAM_*
#include "quant_sqrt/graph_quant_sqrt.h" // QUANT_SQRT_PARAM_*

#include "openzl/zl_compressor.h"
#include "openzl/zl_ctransform.h"
#include "openzl/zl_localParams.h"

#include <string.h>

// Register a predictor and attach the geometry it reads.
static ZL_NodeID geometry_node(ZL_Compressor *c,
                               const ZL_TypedEncoderDesc *desc, uint32_t width,
                               uint32_t planes) {
  ZL_NodeID base = ZL_Compressor_registerTypedEncoder(c, desc);
  if (!ZL_NodeID_isValid(base))
    return base;
  const ZL_IntParam ip[2] = {
      {.paramId = GEOZL_PARAM_WIDTH, .paramValue = (int)width},
      {.paramId = GEOZL_PARAM_PLANES, .paramValue = (int)planes},
  };
  // one plane is what the codec assumes, so leave the param off
  ZL_LocalParams lp = {.intParams = {.intParams = ip,
                                     .nbIntParams = planes > 1 ? 2u : 1u}};
  ZL_NodeParameters np = {.localParams = &lp};
  ZL_RESULT_OF(ZL_NodeID) r = ZL_Compressor_parameterizeNode(c, base, &np);
  return ZL_RES_isError(r) ? ZL_NODE_ILLEGAL : ZL_RES_value(r);
}

// delta_w only reads to its left, so it never crosses a plane boundary.
static ZL_NodeID width_node(ZL_Compressor *c, const ZL_TypedEncoderDesc *desc,
                            uint32_t width) {
  return geometry_node(c, desc, width, 1);
}

ZL_NodeID geozl_node_delta_w(ZL_Compressor *c, uint32_t width) {
  const ZL_TypedEncoderDesc desc = EI_DELTA_W(GEOZL_CTID_DELTA_W);
  return width_node(c, &desc, width);
}
ZL_NodeID geozl_node_delta_n(ZL_Compressor *c, uint32_t width,
                            uint32_t planes) {
  const ZL_TypedEncoderDesc desc = EI_DELTA_N(GEOZL_CTID_DELTA_N);
  return geometry_node(c, &desc, width, planes);
}
ZL_NodeID geozl_node_planar(ZL_Compressor *c, uint32_t width,
                            uint32_t planes) {
  const ZL_TypedEncoderDesc desc = EI_PLANAR(GEOZL_CTID_PLANAR);
  return geometry_node(c, &desc, width, planes);
}
ZL_NodeID geozl_node_planar_zigzag(ZL_Compressor *c, uint32_t width,
                                   uint32_t planes) {
  const ZL_TypedEncoderDesc desc =
      EI_PLANAR_ZIGZAG(GEOZL_CTID_PLANAR_ZIGZAG);
  return geometry_node(c, &desc, width, planes);
}
ZL_NodeID geozl_node_planar_zigzag_pfor(ZL_Compressor *c, uint32_t width,
                                        uint32_t planes) {
  const ZL_TypedEncoderDesc desc =
      EI_PLANAR_ZIGZAG_PFOR(GEOZL_CTID_PLANAR_ZIGZAG_PFOR);
  return geometry_node(c, &desc, width, planes);
}
ZL_NodeID geozl_node_planar_zigzag_transpose_pivco(ZL_Compressor *c,
                                                   uint32_t width,
                                                   uint32_t planes) {
  const ZL_TypedEncoderDesc desc = EI_PLANAR_ZIGZAG_TRANSPOSE_PIVCO(
      GEOZL_CTID_PLANAR_ZIGZAG_TRANSPOSE_PIVCO);
  return geometry_node(c, &desc, width, planes);
}

ZL_NodeID geozl_node_planar_zigzag_pivco(ZL_Compressor *c, uint32_t width,
                                         uint32_t planes) {
  return geozl_node_planar_zigzag_transpose_pivco(c, width, planes);
}

ZL_NodeID geozl_node_transpose_pivco(ZL_Compressor *c) {
  const ZL_TypedEncoderDesc desc =
      EI_TRANSPOSE_PIVCO(GEOZL_CTID_TRANSPOSE_PIVCO);
  return ZL_Compressor_registerTypedEncoder(c, &desc);
}
ZL_NodeID geozl_node_med(ZL_Compressor *c, uint32_t width,
                            uint32_t planes) {
  const ZL_TypedEncoderDesc desc = EI_MED(GEOZL_CTID_MED);
  return geometry_node(c, &desc, width, planes);
}
ZL_NodeID geozl_node_med_zigzag(ZL_Compressor *c, uint32_t width,
                            uint32_t planes) {
  const ZL_TypedEncoderDesc desc = EI_MED_ZIGZAG(GEOZL_CTID_MED_ZIGZAG);
  return geometry_node(c, &desc, width, planes);
}
ZL_NodeID geozl_node_average(ZL_Compressor *c, uint32_t width,
                            uint32_t planes) {
  const ZL_TypedEncoderDesc desc = EI_AVERAGE(GEOZL_CTID_AVERAGE);
  return geometry_node(c, &desc, width, planes);
}
ZL_NodeID geozl_node_wp_static(ZL_Compressor *c, uint32_t width,
                            uint32_t planes) {
  const ZL_TypedEncoderDesc desc = EI_WP_STATIC(GEOZL_CTID_WP_STATIC);
  return geometry_node(c, &desc, width, planes);
}

ZL_NodeID geozl_node_deinterleave(ZL_Compressor *c) {
  const ZL_TypedEncoderDesc desc = EI_DEINTERLEAVE(GEOZL_CTID_DEINTERLEAVE);
  return ZL_Compressor_registerTypedEncoder(c, &desc);
}

ZL_NodeID geozl_node_pfor(ZL_Compressor *c) {
  const ZL_TypedEncoderDesc desc = EI_PFOR(GEOZL_CTID_PFOR);
  return ZL_Compressor_registerTypedEncoder(c, &desc);
}

ZL_NodeID geozl_node_binoffset(ZL_Compressor *c) {
  (void)c;
  return ZL_NODE_ILLEGAL;
}

ZL_NodeID geozl_node_intmult(ZL_Compressor *c, uint64_t base) {
  (void)c;
  (void)base;
  return ZL_NODE_ILLEGAL;
}

ZL_NodeID geozl_node_floatquant(ZL_Compressor *c, unsigned k) {
  (void)c;
  (void)k;
  return ZL_NODE_ILLEGAL;
}

ZL_NodeID geozl_node_floatmult(ZL_Compressor *c, double base) {
  (void)c;
  (void)base;
  return ZL_NODE_ILLEGAL;
}

ZL_NodeID geozl_node_nodata(ZL_Compressor *c, uint32_t width,
                            geozl_nodata_mode mode, uint64_t valueBits,
                            int dtype, double radius) {
  // Sentinel mode needs the dtype to order samples around the sentinel.
  if (mode != GEOZL_NODATA_NAN && mode != GEOZL_NODATA_VALUE)
    return ZL_NODE_ILLEGAL;
  const int guard = mode == GEOZL_NODATA_VALUE;
  if (guard && !GEOZL_DT_OK(dtype))
    return ZL_NODE_ILLEGAL;
  const ZL_TypedEncoderDesc desc = EI_NODATA(GEOZL_CTID_NODATA);
  ZL_NodeID base = ZL_Compressor_registerTypedEncoder(c, &desc);
  if (!ZL_NodeID_isValid(base))
    return base;
  // The pattern is 8 bytes whatever the sample width, a copy param rather than
  // an int param because an int cannot carry a 64-bit double's bits.
  uint8_t bits[8];
  geozl_st_le64(bits, valueBits);
  const ZL_IntParam ip[3] = {
      {.paramId = GEOZL_NODATA_PARAM_WIDTH, .paramValue = (int)width},
      {.paramId = GEOZL_NODATA_PARAM_MODE, .paramValue = (int)mode},
      {.paramId = GEOZL_NODATA_PARAM_DTYPE, .paramValue = dtype},
  };
  uint8_t rad[8];
  geozl_st_le_f64(rad, radius);
  const ZL_CopyParam cp[2] = {
      {.paramId = GEOZL_NODATA_PARAM_VALUE,
       .paramPtr = bits,
       .paramSize = sizeof(bits)},
      {.paramId = GEOZL_NODATA_PARAM_RADIUS,
       .paramPtr = rad,
       .paramSize = sizeof(rad)},
  };
  ZL_LocalParams lp = {
      .intParams = {.intParams = ip, .nbIntParams = guard ? 3u : 2u},
      .copyParams = {.copyParams = cp, .nbCopyParams = 2},
  };
  ZL_NodeParameters np = {.localParams = &lp};
  ZL_RESULT_OF(ZL_NodeID) r = ZL_Compressor_parameterizeNode(c, base, &np);
  return ZL_RES_isError(r) ? ZL_NODE_ILLEGAL : ZL_RES_value(r);
}

ZL_NodeID geozl_node_quant_linear(ZL_Compressor *c,
                                  const quant_linear_params *params,
                                  int dtype) {
  const ZL_TypedEncoderDesc desc = EI_QUANT_LINEAR(GEOZL_CTID_QUANT_LINEAR);
  ZL_NodeID base = ZL_Compressor_registerTypedEncoder(c, &desc);
  if (!ZL_NodeID_isValid(base))
    return base;
  const ZL_IntParam ip = {.paramId = QUANT_LINEAR_PARAM_DTYPE,
                          .paramValue = dtype};
  const ZL_CopyParam cp = {.paramId = QUANT_LINEAR_PARAM_PARAMS,
                           .paramPtr = params,
                           .paramSize = sizeof(*params)};
  ZL_LocalParams lp = {
      .intParams = {.intParams = &ip, .nbIntParams = 1},
      .copyParams = {.copyParams = &cp, .nbCopyParams = 1},
  };
  ZL_NodeParameters np = {.localParams = &lp};
  ZL_RESULT_OF(ZL_NodeID) r = ZL_Compressor_parameterizeNode(c, base, &np);
  return ZL_RES_isError(r) ? ZL_NODE_ILLEGAL : ZL_RES_value(r);
}

ZL_NodeID geozl_node_quant_log(ZL_Compressor *c, const quant_log_params *params,
                               int dtype) {
  const ZL_TypedEncoderDesc desc = EI_QUANT_LOG(GEOZL_CTID_QUANT_LOG);
  ZL_NodeID base = ZL_Compressor_registerTypedEncoder(c, &desc);
  if (!ZL_NodeID_isValid(base))
    return base;
  const ZL_IntParam ip = {.paramId = QUANT_LOG_PARAM_DTYPE,
                          .paramValue = dtype};
  const ZL_CopyParam cp = {.paramId = QUANT_LOG_PARAM_PARAMS,
                           .paramPtr = params,
                           .paramSize = sizeof(*params)};
  ZL_LocalParams lp = {
      .intParams = {.intParams = &ip, .nbIntParams = 1},
      .copyParams = {.copyParams = &cp, .nbCopyParams = 1},
  };
  ZL_NodeParameters np = {.localParams = &lp};
  ZL_RESULT_OF(ZL_NodeID) r = ZL_Compressor_parameterizeNode(c, base, &np);
  return ZL_RES_isError(r) ? ZL_NODE_ILLEGAL : ZL_RES_value(r);
}

ZL_NodeID geozl_node_quant_sqrt(ZL_Compressor *c,
                                const quant_sqrt_params *params, int dtype) {
  const ZL_TypedEncoderDesc desc = EI_QUANT_SQRT(GEOZL_CTID_QUANT_SQRT);
  ZL_NodeID base = ZL_Compressor_registerTypedEncoder(c, &desc);
  if (!ZL_NodeID_isValid(base))
    return base;
  const ZL_IntParam ip = {.paramId = QUANT_SQRT_PARAM_DTYPE,
                          .paramValue = dtype};
  const ZL_CopyParam cp = {.paramId = QUANT_SQRT_PARAM_PARAMS,
                           .paramPtr = params,
                           .paramSize = sizeof(*params)};
  ZL_LocalParams lp = {
      .intParams = {.intParams = &ip, .nbIntParams = 1},
      .copyParams = {.copyParams = &cp, .nbCopyParams = 1},
  };
  ZL_NodeParameters np = {.localParams = &lp};
  ZL_RESULT_OF(ZL_NodeID) r = ZL_Compressor_parameterizeNode(c, base, &np);
  return ZL_RES_isError(r) ? ZL_NODE_ILLEGAL : ZL_RES_value(r);
}

ZL_NodeID geozl_node_lossy(ZL_Compressor *c, const geozl_lossy_plan *plan,
                           int dtype) {
  switch (plan->family) {
  case GEOZL_LOSSY_LINEAR:
    return geozl_node_quant_linear(c, &plan->as.linear, dtype);
  case GEOZL_LOSSY_LOG:
    return geozl_node_quant_log(c, &plan->as.log, dtype);
  case GEOZL_LOSSY_SQRT:
    return geozl_node_quant_sqrt(c, &plan->as.sqrt, dtype);
  case GEOZL_LOSSY_NONE:
    break;
  }
  return ZL_NODE_ILLEGAL;
}
