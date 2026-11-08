#ifndef GEOZL_CODECS_LANES_DECODE_KERNEL_H
#define GEOZL_CODECS_LANES_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

int lanes_join(void *dst, const uint8_t *lanes, size_t count,
               size_t width);

#endif // GEOZL_CODECS_LANES_DECODE_KERNEL_H
