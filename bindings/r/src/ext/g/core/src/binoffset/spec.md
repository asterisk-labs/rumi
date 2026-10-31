# binoffset Decoder Specification

Lossless numeric codec, CTID `0x72D708`.

## Inputs

Two numeric streams with the same element count:

- `bins`, with one-byte elements.
- `offsets`, with elements 1, 2, 4, or 8 bytes wide.

## Codec header

Little endian. Byte 0 is `nbBins`, in `1..255`. It is followed by `nbBins`
entries containing a lower bound in `eltWidth` bytes and a one-byte
`offset_bits` value. Each `offset_bits` must be at most `8 * eltWidth`.

The header length is exactly `1 + nbBins * (eltWidth + 1)` bytes.

## Decoding

For each unsigned input pair:

    v = lower[bins[n]] + offsets[n]    modulo 2^(8*eltWidth)

A pair is valid when `bins[n] < nbBins` and the offset fits in the selected
bin's `offset_bits`. Every offset is valid when that value equals the full
element width. Invalid pairs make the frame corrupt.

The decoder does not require sorted, contiguous, or non-overlapping bins.

## Output

One numeric stream with the width and element count of `offsets`.
