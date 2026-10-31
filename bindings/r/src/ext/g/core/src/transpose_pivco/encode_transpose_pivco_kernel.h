#ifndef GEOZL_CODECS_TRANSPOSE_PIVCO_ENCODE_KERNEL_H
#define GEOZL_CODECS_TRANSPOSE_PIVCO_ENCODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

int transpose_pivco_lanes(uint8_t *lanes, const void *src, size_t count,
                          size_t width);

#endif // GEOZL_CODECS_TRANSPOSE_PIVCO_ENCODE_KERNEL_H
