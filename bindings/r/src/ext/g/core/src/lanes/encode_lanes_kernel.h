#ifndef GEOZL_CODECS_LANES_ENCODE_KERNEL_H
#define GEOZL_CODECS_LANES_ENCODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

int lanes_split(uint8_t *lanes, const void *src, size_t count,
                size_t width);

#endif // GEOZL_CODECS_LANES_ENCODE_KERNEL_H
