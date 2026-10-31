# quant_log Decoder Specification

Lossy numeric codec, CTID `0x72D782`.

`quant_log` uses a logarithmic grid with a relative error bound:

    LOG:MAX_ERROR=V%

`MAX_ERROR` is required, must include `%`, and must be in `(0, 100)`. Unknown or
repeated keys and trailing commas are rejected.

The dtype picks what the stream carries. Integer input stores the reconstructed
values: the encoder rounds each level to a whole number, so the decoder never
rebuilds a level and rounds it a second time, which the bound has no budget for.
Floating-point input stores grid indices. A float frame from 0.14 to 0.18 may hold
values, which that release's `STORE=VALUES` asked for; a decoder still reads it.

## Inputs

One numeric stream with the element count and width of the original type.

## Codec header

Exactly 10 little-endian bytes:

- Byte 0: original type. Codes 0..3 are u8, u16, u32, and u64; 4..7 are the
  signed types of the same widths; 8..10 are f16, f32, and f64.
- Byte 1: flags. Bit 0 means the source contained no negative sample. Bit 1
  means the stream stores reconstructed values rather than indices.
- Bytes 2..9: `step`, the grid width in log2 space, as IEEE binary64.

A frame is corrupt if the type or flags are unknown, `step` is not positive and
finite, or an integer frame stores indices.

## Representation and decoding

The encoded stream always has an integer representation:

| Input | Storage | Reconstruction |
| --- | --- | --- |
| integer | values | copy the stored value |
| float | values (0.14 to 0.18) | cast the stored value to the output type |
| float | index | rebuild the logarithmic level |

The values grid is anchored at one. Its levels are rounded to whole numbers
before encoding:

    level(j) = round(2^(j * step))

For an index `q`, zero reconstructs as zero. A nonzero index reconstructs as:

    sign(q) * 2^(A + (abs(q) - 1) * step)

where `A` is -24 for f16, -149 for f32, and -1074 for f64. The result is
clamped to the output range and to zero when flag bit 0 is set.

## Resolving the grid

The resolver chooses `step` so grid error and output rounding remain within the
requested relative bound. The values grid of integer input splits that budget
between the grid and whole-number rounding.

The index grid covers the full floating-point type rather than the current tile.
It is refused if the requested bound is below the output precision or needs
more indices than the stream width can hold. Its anchor and step do not depend
on tile statistics. Tile statistics only set the nonnegative flag.

Zero is exact. Negative zero loses its sign. NaNs reconstruct as zero unless a
preceding `nodata` codec restores them. Infinities are clamped to the largest
finite reconstruction.

For floating-point input, the relative-error guarantee starts at the smallest
normal value. Subnormal values are still encoded, but the high-level resolver
does not reject or report them and no constant relative bound is guaranteed for
them.

The encoder and decoder use the deterministic `log2` and `exp2` routines in
`quant_log_math.h`; they require floating-point contraction to be disabled for
bitwise reproducibility.

## Output

One numeric stream with the original type and element count. For every finite,
nonzero input in the guaranteed domain:

    |x - reconstructed(x)| <= (MAX_ERROR / 100) * |x|
