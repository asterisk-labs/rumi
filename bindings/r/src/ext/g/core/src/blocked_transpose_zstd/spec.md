# blocked_transpose_zstd

The transform accepts one numeric stream and emits one serial stream. It splits
the input into fixed-size blocks, byte-transposes each block into
least-significant-byte-first lanes, and compresses every shuffled block as an
independent magicless Zstandard frame.

The 16-byte little-endian codec header is:

- byte 0: format version (`1`)
- byte 1: numeric element width (`1`, `2`, `4`, or `8`)
- bytes 2-3: reserved zero
- bytes 4-7: uncompressed block size in bytes
- bytes 8-15: element count

The serial payload repeats a little-endian `uint32` compressed size followed by
that many bytes of Zstandard data. The final block may be shorter. Zstandard
content size, checksum, and magic are omitted because the codec header and block
prefixes already frame the data; the enclosing OpenZL frame may carry its own
checksums.
