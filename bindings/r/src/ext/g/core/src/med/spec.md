# med Decoder Specification

Lossless numeric codec, CTID `0x72D705`.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit MED residuals in row-major order.

## Codec header

Little endian:

- bytes 0-3: row width in samples, as `uint32`;
- bytes 4-7: optional plane count, as `uint32`.

A four-byte header means one plane. The encoder writes eight bytes only when
the plane count is greater than one. Width and plane count must be nonzero, and
each plane must contain whole rows.

## Decoding

Planes are decoded independently. Let `W`, `N` and `NW` be reconstructed
neighbors, with zero outside the plane. The MED prediction is

    min(W, N)       if NW >= max(W, N)
    max(W, N)       if NW <= min(W, N)
    W + N - NW      otherwise

The output is the residual plus the prediction, with arithmetic wrapping at the
element width.

## Output

One numeric stream with the input length and element width.
