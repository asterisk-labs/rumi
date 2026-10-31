# quant_linear Decoder Specification

Lossy numeric codec, CTID `0x72D781`.

`quant_linear` uses a zero-anchored uniform grid with an absolute error bound:

    LINEAR:MAX_ERROR=V

`MAX_ERROR` is required and must be positive. The encoder writes grid indices.
Frames written from 0.14 to 0.18 may hold reconstructed values instead, which the
`STORE=VALUES` key of those releases asked for; a decoder still reads them.

## Inputs

One numeric stream with the element count and width of the original type.

## Codec header

Exactly 10 little-endian bytes:

- Byte 0: original type. Codes 0..3 are u8, u16, u32, and u64; 4..7 are the
  signed types of the same widths; 8..10 are f16, f32, and f64.
- Byte 1: flags. Bit 0 means the source contained no negative finite sample.
  Bit 1 means the stream stores reconstructed values rather than indices.
- Bytes 2..9: `step` as IEEE binary64.

A frame is corrupt if the type or flags are unknown, `step` is smaller than
`DBL_MIN` or is not finite, or the step is fractional on any frame other than a
floating-point index frame. An integer frame rebuilds in integer arithmetic
whichever way it stores, so it needs a whole step both ways; only the
floating-point index path carries a fractional one.

## Representation and decoding

The encoded stream always has an integer representation:

| Input | Storage | Stream value | Reconstruction |
| --- | --- | --- | --- |
| integer | index | grid index `q` | `q * step`, in integer arithmetic |
| integer | values (0.14 to 0.18) | reconstructed value | copied |
| float | values (0.14 to 0.18) | quantized value `q` | cast `q` to the output type |
| float | index | grid index `q` | `q * step` |

The result is clamped to the output type. When flag bit 0 is set, the lower
limit is zero. Reconstruction is rounded once to the output width.

An integer index rebuilds with integers, not doubles: take `step` to a whole
number the way the encoder did, multiply the magnitude, put the sign back, then
clamp. A decoder that multiplies in floating point instead will disagree with
the encoder above the exact-integer range of a double, so the bound stops
holding for u64 and i64. Saturate before multiplying rather than after: a forged
index against a wide step overflows the product, and a wrapped product can land
back inside the output range as a plausible wrong number.

## Resolving the grid

For integer input:

    step = floor(2 * MAX_ERROR)

with a minimum of one, which is lossless. A values frame from 0.14 to 0.18 used
the same grid, and a floating-point one the same whole step.

An integer index needs no range check. The encoder forms the magnitude index as
`(|x| + step/2) / step` in integer arithmetic, which for a step of one or more is
never above `|x|`, so an index always fits the stream the value came from.

For floating-point input, division during encoding and multiplication during
decoding both consume rounding budget:

    step = 2 * (MAX_ERROR - 2 * eps * maxAbs)

Here `eps` is `2^-11`, `2^-24`, or `2^-53` for f16, f32, or f64. The recipe is
refused if the step is not positive or the required index exceeds the stream
range.

All paths scan whether finite samples are negative. Only the floating-point
path makes the grid depend on `maxAbs`, so changing tile boundaries can change
its reconstruction while preserving the declared bound.

Non-finite samples are outside the error guarantee and should be handled by the
`nodata` codec.

## Output

One numeric stream with the original type and element count. For every finite
input covered by the resolved grid:

    |x - reconstructed(x)| <= MAX_ERROR
