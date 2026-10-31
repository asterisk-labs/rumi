#ifndef GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_H
#define GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_H

#include "geozl/export.h"

#include <stddef.h>

GEOZL_API int geozl_blocked_transpose_zstd_decode(
    void *dst, size_t nbElts, size_t eltWidth, size_t blockSize,
    const void *src, size_t srcSize);

#endif // GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_H
