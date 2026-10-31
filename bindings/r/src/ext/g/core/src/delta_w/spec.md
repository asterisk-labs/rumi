# delta_w Decoder Specification

Lossless numeric codec, CTID `0x72D701`.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit horizontal residuals in row-major
order.

## Codec header

Exactly four bytes: the row width in samples as a little-endian `uint32`.
Width must be nonzero and divide the element count.

## Decoding

Each row is decoded independently. Its first sample is absolute; later samples
use

    output[y, x] = residual[y, x] + output[y, x - 1]

Addition wraps at the element width.

## Output

One numeric stream with the input length and element width.
