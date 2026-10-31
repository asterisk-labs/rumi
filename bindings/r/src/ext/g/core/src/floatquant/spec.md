# floatquant Decoder Specification

Lossless floating-point codec, CTID `0x72D70A`.

## Inputs

Two streams with the same count and element width, either 4 or 8 bytes:

- `primary`, the float order key without its low `k` bits.
- `secondary`, the low bits, reversed for negative values.

## Codec header

Exactly one byte containing `k`. It must be in `1..23` for binary32 and
`1..52` for binary64.

## Decoding

Let `SIGN` be the sign bit and `maxlow = (1 << k) - 1`. For each pair:

    is_pos = primary >= (SIGN >> k)
    low    = is_pos ? secondary : maxlow - secondary
    key    = (primary << k) + low
    bits   = (key & SIGN) ? key ^ SIGN : ~key

`secondary` must not exceed `maxlow`; otherwise the frame is corrupt. The
output is the floating-point reinterpretation of `bits`, preserving all bit
patterns including infinities and NaNs.

## Output

One floating-point stream with the input width and element count.
