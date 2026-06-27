#ifndef RUMI_REGISTER_LOSSY_H
#define RUMI_REGISTER_LOSSY_H

#include "openzl/zl_dtransform.h" // ZL_DCtx
#include "openzl/zl_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers every rumi.lossy decoder into the given DCtx. These codecs are lossy,
// so a frame that uses one is not bit exact. Only a reader that registers them,
// like rumi, reconstructs it.
ZL_Report rumi_register_lossy_decoders(ZL_DCtx* dctx);

#ifdef __cplusplus
}
#endif

#endif // RUMI_REGISTER_LOSSY_H
