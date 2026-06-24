#ifndef RUMI_REGISTER_EXPERIMENTAL_H
#define RUMI_REGISTER_EXPERIMENTAL_H

#include "openzl/zl_dtransform.h" // ZL_DCtx, ZL_Report

#ifdef __cplusplus
extern "C" {
#endif

// Registers the experimental rumi predictor decoders into dctx. Decoders are
// stateless so one set per dctx per thread is fine. Returns the first error or
// success.
ZL_Report rumi_register_experimental_decoders(ZL_DCtx* dctx);

#ifdef __cplusplus
}
#endif

#endif // RUMI_REGISTER_EXPERIMENTAL_H