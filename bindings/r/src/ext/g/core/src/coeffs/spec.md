# Frame coefficient format

Coefficient blobs are stored in OpenZL's frame header comment. They are not a
codec and have no CTid. Readers can access them without decompressing the frame.
Header comments exist from OpenZL frame format version 22; older frames have no
coefficient vectors.

geozl treats the contents as numeric vectors; their meaning belongs to the
producer and consumer.

## Layout

All integers are little-endian, two's complement.

    bytes   field
    0..3    "GZC1"
    4       format version (1)
    5       vector count (1..255)

Each vector then contains a `uint32` value count followed by that many `int32`
values. Empty vectors are invalid.

The record lengths must consume the blob exactly. Truncation, trailing bytes,
a vector count of zero, an unknown version, or a total above 10000 bytes makes
a blob invalid. Accumulate those lengths in more than 32 bits: 255 vectors of
4294967295 values each overflow a `uint32` and would otherwise wrap into a
total that fits. Every value in range round-trips; the format imposes no other
constraint.

A missing comment or a comment without the `GZC1` magic means that the frame has
no geozl coefficient vectors. A comment with the magic but an invalid body is
reported as corruption.

The encoded size is 6 bytes, plus 4 bytes per vector and 4 bytes per value. The
10000-byte OpenZL limit allows at most 2497 values in one vector.
