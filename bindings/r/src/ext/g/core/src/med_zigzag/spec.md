# med_zigzag Codec Specification

Lossless numeric codec, CTID `0x72D711`. Numeric in, numeric out. It writes
exactly what `med` followed by OpenZL Zigzag writes, in one pass instead of
two.

Each residual is Zigzag-mapped before it is written to the output. The separate
`med` and Zigzag nodes produce the same values but also produce and consume an
intermediate residual stream.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit samples in row-major order.

## Codec header

Little endian:

- bytes 0-3: row width in samples, as `uint32`;
- bytes 4-7: optional plane count, as `uint32`.

A four-byte header means one plane. The encoder writes eight bytes only when
the plane count is greater than one. Width and plane count must be nonzero, and
each plane must contain whole rows.

## Encoding

Planes are independent. Let `W`, `N` and `NW` be source neighbours, with zero
outside the plane. The MED prediction is the JPEG-LS median edge detector,
ITU-T T.87 A.4.2:

    min(W, N)       if NW >= max(W, N)
    max(W, N)       if NW <= min(W, N)
    W + N - NW      otherwise

Arithmetic wraps at the element width. The gradient `W + N - NW` is used only
when it lies inside `[min(W, N), max(W, N)]`, so it never leaves the width.

    residual = sample - prediction

Interpret the residual bits as a two's-complement integer of the same width and
map it to unsigned Zigzag form:

    encoded = (residual << 1) ^ sign_mask(residual)

`sign_mask` is the residual's sign broadcast over the whole word: all ones for a
negative residual, zero otherwise. On a `W`-bit value it is the arithmetic right
shift `residual >> (W - 1)`. The mapping sends 0, -1, 1, -2 to 0, 1, 2, 3, so a
residual near zero stays a small unsigned number whichever way it leans, which
is what the entropy stage that follows wants.

The output is identical to `med` followed by OpenZL Zigzag.

## Decoding

First recover each residual:

    residual = (encoded >> 1) ^ -(encoded & 1)

Then reconstruct the plane, with the prediction read off the already
reconstructed neighbours:

    sample = residual + prediction

Unlike `planar`, the MED prediction is not a prefix sum: the median makes it an
IIR through `W`, so a row cannot be vectorized. A decoder is free to reorder the
traversal, and the reference implementation advances four rows at a time, each
one column behind the row above, so the row chains run concurrently. The
reconstructed values do not depend on that choice.

## Output

One numeric stream with the input length and element width.

## Relation to med

`med` (CTID `0x72D705`) stops after the predictor and leaves a signed residual
for a following Zigzag node. This codec also applies Zigzag and is still
numeric out, so an entropy or packing stage follows it. The values it emits are
the ones the two-node graph emits.

## Compatibility

Recipes beginning with `med>zigzag` select this codec. Decoders register both
`med` and `med_zigzag`, so frames written by the two-node graph remain
readable.
