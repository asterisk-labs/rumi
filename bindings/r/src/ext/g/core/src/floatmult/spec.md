# floatmult Decoder Specification

Lossless floating-point codec, CTID `0x72D70B`.

## Inputs

Two streams with the same count and element width, either 4 or 8 bytes:

- `primary`, a multiplier stored through the integer-float latent.
- `secondary`, a centered ULP adjustment from `mult * base`.

## Codec header

Exactly 8 little-endian bytes containing `base` as an IEEE binary64 value.
`base` must be finite and nonzero. The 4-byte path narrows it to binary32.

## Decoding

Let `ord` be the total-order key for the input width and `MID` its sign bit.
After decoding `primary` to the floating-point value `mult`:

    prod = mult * base
    key  = ord(prod) + (secondary - MID)    modulo the element width
    x    = inverse_ord(key)

The multiplication uses the stream's own precision. The transform preserves
the bit pattern of finite values, infinities, and NaNs. The frame checksum is
responsible for detecting per-element corruption.

## Output

One floating-point stream with the input width and element count.
