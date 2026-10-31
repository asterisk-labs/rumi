# intmult Decoder Specification

Lossless integer codec, CTID `0x72D709`.

## Inputs

Two numeric streams with the same count and element width of 1, 2, 4, or 8
bytes:

- `mults`, the unsigned quotient by `base`.
- `adjs`, the unsigned remainder.

## Codec header

Exactly `eltWidth` little-endian bytes containing `base`. The base must be at
least 2.

## Decoding

For each pair:

    u = mults[n] * base + adjs[n]    modulo 2^(8*eltWidth)

`adjs[n]` must be less than `base`; otherwise the frame is corrupt. For valid
encoder output, quotient and remainder reconstruct the original bit pattern.

## Output

One numeric stream with the input width and element count.
