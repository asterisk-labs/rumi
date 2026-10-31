#ifndef GEOZL_CODECS_TRANSPOSE_PIVCO_DECODE_KERNEL_H
#define GEOZL_CODECS_TRANSPOSE_PIVCO_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

int transpose_pivco_unlanes(void *dst, const uint8_t *lanes, size_t count,
                            size_t width);

#endif // GEOZL_CODECS_TRANSPOSE_PIVCO_DECODE_KERNEL_H
