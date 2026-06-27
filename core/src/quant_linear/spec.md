## Quant-Linear Decoder Specification

The 'quant_linear' codec is lossy, in the `rumi.lossy` namespace, CTid `0x72D780`. It
is uniform near lossless quantization. The encoder produces integer indices with
`q = round(x / scale)`, which discards the within step remainder, so the decoder does
not reconstruct the original stream exactly. The error is bounded, see Error bound.

It works on unsigned integer, signed integer, and IEEE float of 16, 32, or 64 bits.
The original element type is not carried by the numeric stream, which only knows its
width, so it travels in the codec header, the same way OpenZL's float_deconstruct
carries its element type.

### Inputs
The decoder takes a single numeric stream of integer indices. The index element width
is the same as the original element width, 1, 2, 4, or 8 bytes. For integer originals
the index is the same integer type. For float originals the index is a signed integer
of the same byte width, f16 to int16, f32 to int32, f64 to int64.

### Codec Header
The codec header is 9 bytes, little-endian.

- byte 0, the original element type, an enum.
  - 0 u8, 1 u16, 2 u32, 3 u64
  - 4 i8, 5 i16, 6 i32, 7 i64
  - 8 f16, 9 f32, 10 f64
- bytes 1 to 8, the quantization step `scale`, an IEEE double. `scale = 2 * max_error`.
  For integer originals `scale` is an integer value stored in the double. A `scale` of
  0 is an exact passthrough.

### Decoding
The decode reads the type and the scale from the header, then reconstructs each value.

- Integer original, `x = q * scale`, clamped to the type range so the top does not wrap.
- Float original, `x = q * scale` computed in double and cast to the float width.

Consider an integer stream {0, 1, 3} with type u16 and scale 10. The decoded stream is
{0, 10, 30} as u16.

### Outputs
A single numeric stream of the original element type and width, with the same number of
elements as the input.

### Error bound
The encoder rounds each value to the nearest multiple of `scale`, and `scale` is
`2 * max_error`, so every decoded value is within `max_error` of the original. For
integer types this is exact. For float types the decoded value is `q * scale` rounded
to the float width, which can add up to half a ULP of that float type on top of
`max_error`. This extra is inherent to the float representation and negligible.

The step applies to the whole tile. There is no per block minimum, because in rumi the
spatial predictors remove the local offset, so the per block minimum that LERC needs is
not used here.
