#ifndef GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_GRAPH_H
#define GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_GRAPH_H

#include "openzl/zl_data.h" // ZL_Type_*, ZL_STREAMTYPELIST

#define BLOCKED_TRANSPOSE_ZSTD_GRAPH(id)                                      \
  {                                                                            \
    .CTid = (id), .inStreamType = ZL_Type_numeric,                             \
    .outStreamTypes = ZL_STREAMTYPELIST(ZL_Type_serial)                        \
  }

#define BLOCKED_TRANSPOSE_ZSTD_PARAM_BLOCK_SIZE 1

#define BLOCKED_TRANSPOSE_ZSTD_VERSION 1
#define BLOCKED_TRANSPOSE_ZSTD_HEADER_SIZE 16
#define BLOCKED_TRANSPOSE_ZSTD_DEFAULT_BLOCK_SIZE (2u * 1024u * 1024u)
#define BLOCKED_TRANSPOSE_ZSTD_MAX_BLOCK_SIZE (64u * 1024u * 1024u)

#endif // GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_GRAPH_H
