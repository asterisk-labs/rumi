// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pcodec's bin and offset reconstruction, the inverse of
// pco/src/histograms.rs and the write_short_uints packer. Bounds and range
// errors accumulate into a return flag instead of a Rust Result. The port is
// partial and geozl does not reproduce pcodec's wire format.

#ifndef GEOZL_CODECS_BINOFFSET_DECODE_KERNEL_H
#define GEOZL_CODECS_BINOFFSET_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// v = bin == 0 ? 0 : (1 << (bin - 1)) + offset, for nb_elts elements of
// elt_width bytes (1, 2, 4 or 8). Returns nonzero if any bin exceeds the lane
// width or any offset is not below 1 << (bin - 1). On nonzero, dst is garbage
// but every write stayed in bounds.
int binoffset_join(void *dst, const uint8_t *bins, const void *offsets,
                   size_t nb_elts, size_t elt_width);

// v = lowers[bin] + offset. Both tables padded to 256 so a corrupt bin id
// loads in bounds; the flag rejects it afterwards. Returns nonzero if any bin
// id is not below nb_bins or any offset reaches 1 << offset_bits[bin]. On
// nonzero, dst is garbage but every access stayed in bounds.
int binoffset_join_table(void *dst, const uint8_t *bins, const void *offsets,
                         size_t nb_elts, size_t elt_width,
                         const uint64_t lowers[256],
                         const uint8_t offset_bits[256], unsigned nb_bins);


// inverse of binoffset_split_pack. Returns nonzero on a corrupt bin id or if
// reads run past packed_len.
int binoffset_unpack_join(void *dst, const uint8_t *bins, const uint8_t *packed,
                          size_t packed_len, size_t nb_elts, size_t elt_width,
                          const uint64_t lowers[256],
                          const uint8_t offset_bits[256], unsigned nb_bins);

#endif // GEOZL_CODECS_BINOFFSET_DECODE_KERNEL_H
