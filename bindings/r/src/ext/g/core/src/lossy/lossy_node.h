#ifndef GEOZL_LOSSY_NODE_H
#define GEOZL_LOSSY_NODE_H

#include "lossy_recipe.h"

#include "openzl/zl_compressor.h"

// Separate from lossy_recipe.h, which goes into the kernels library, because
// this one needs OpenZL. Defined in encoder_registry.c, next to the three
// builders it delegates to. A NONE plan is lossless and has no node, so this
// returns ZL_NODE_ILLEGAL for it.
ZL_NodeID geozl_node_lossy(ZL_Compressor *c, const geozl_lossy_plan *plan,
                           int dtype);

#endif // GEOZL_LOSSY_NODE_H