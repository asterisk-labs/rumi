#ifndef GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_DECODE_KERNEL_H
#define GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_DECODE_KERNEL_H

#include <stddef.h>

// Reverse blocked_transpose_zstd_shuffle into native-endian fixed-width values.
void blocked_transpose_zstd_unshuffle(void *dst, const void *src,
                                      size_t nbElts, size_t eltWidth);

#endif // GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_DECODE_KERNEL_H
