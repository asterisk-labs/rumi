# rumi

- Specification 0.1.0
- Status Draft
- Date 2026-08-25
- License GPLv3

rumi is a GeoTIFF-inspired raster format for compressed raster tensors with up
to four dimensions. It stores either an Image `(B, Y, X)` or a temporal Cube
`(T, B, Y, X)` in one tiled file.

The format has the following properties:

- **Stateless reads.** The external binary header locates every compressed frame
  without parsing the `.rumi` file or retaining state between requests.
- **OpenZL compression.** Stores each frame as an independent, self-contained
  OpenZL frame.
- **Predictable layout.** Files with the same band and frame counts begin their
  frame data at the same byte.
- **Time coordinates.** Stores instants or intervals in a compact trailer when
  the time axis is labelled.
- **Canonical structure.** Restricts the file to one fixed IFD, one frame order,
  and contiguous frame data.
- **Fixed-size georeferencing.** Stores an EPSG CRS and affine transform in a
  160-byte block derived from GeoTIFF tags, with a defined value for
  ungeoreferenced rasters.

The filename extension for the format is `.rumi`.

![A raster array is encoded as a rumi file and a rumi header blob](img/rumi-layout.svg)

The key words MUST, MUST NOT, SHOULD, and MAY are to be interpreted as described
in RFC 2119 when, and only when, they appear in capitals.

## Scope

This document defines:

- the rumi data model and frame layouts;
- the GeoTIFF-inspired rumi file profile;
- the time coordinates a Cube carries; and
- the binary layout of the external rumi header blob.

It does not define the OpenZL frame format or a catalogue format.

This document contains everything needed to implement a rumi reader. The only
supported writer is the one provided by rumi; independent writers are outside
the [compatibility policy](https://github.com/asterisk-labs/rumi/blob/main/COMPATIBILITY.md).

Unless a section says otherwise, all integer arithmetic used to validate or
derive sizes, counts, and offsets is exact. A reader MUST reject an input when a
required result cannot be represented by its implementation.

All multi-byte numeric values defined by rumi are little-endian. This includes
the file header, directory entries and values, decoded sample components, the
time trailer, and the external header blob. OpenZL defines the bytes inside a
compressed frame; after decoding, each multi-byte sample component is
little-endian. An API may convert decoded samples to the host's native byte
order.

## Data model

rumi uses the following data model.

![Deep Learning Raster Data Model](img/rumi-data-model.svg)

| term            | shape                            | definition                                        |
| --------------- | -------------------------------- | ------------------------------------------------- |
| tile            | `(h, w)`                         | one band and time step at one tile location       |
| cell            | `(B, h, w)` or `(T, B, h, w)`    | all samples at one tile location                  |
| Image           | `(B, Y, X)`                      | one raster grid; a rumi file with `T = 1`         |
| Cube            | `(T, B, Y, X)`                   | `T` time-ordered, grid-aligned Images in one file |
| ImageCollection | —                                | a set of Images that need not share a grid        |
| CubeCollection  | —                                | a set of Cubes that need not share a grid         |

`T`, `B`, `Y`, and `X` denote time count, band count, image length, and image
width. `h` and `w` denote the actual dimensions of a tile; they may be smaller
than the nominal tile dimensions at the image boundary.

The coordinates of a Cube's time steps are defined in
[Time coordinates](#time-coordinates).

Collections are represented outside the file, for example by a catalogue of
rumi files. Their representation is out of scope.

## Frames

`tile` and `cell` belong to the logical raster model. A frame is the physical
unit of compression and random access. Each frame is a self-contained OpenZL
frame that carries its own graph and codec parameters; neither is stored in the
IFD or external header. Its decoded shape and sample order are specified by
[`frame_unit`](#frame_unit).

A frame MUST contain exactly one of:

- one tile for one `(b, t)` pair at one tile location; or
- one cell at one tile location.

![Tiles and cells are data model units; a frame is their compressed storage unit](img/rumi-model-to-frame.svg)

### frame_unit

`frame_unit` selects one of the decoded layouts below. `b`, `t`, `h`, and `w`
mean band, time, height, and width. The rightmost axis changes fastest.

| frame_unit | decoded frame      | valid when                          |
| ---------- | ------------------ | ----------------------------------- |
| `0`        | `h w`              | any `B` and `T`                     |
| `1`        | `b h w` or `t h w` | exactly one of `B`, `T` exceeds `1` |
| `2`        | `h w b` or `h w t` | exactly one of `B`, `T` exceeds `1` |
| `3`        | `b t h w`          | `B > 1` and `T > 1`                 |
| `4`        | `t b h w`          | `B > 1` and `T > 1`                 |
| `5`        | `b h w t`          | `B > 1` and `T > 1`                 |
| `6`        | `t h w b`          | `B > 1` and `T > 1`                 |
| `7`        | `h w b t`          | `B > 1` and `T > 1`                 |
| `8`        | `h w t b`          | `B > 1` and `T > 1`                 |
| `9`        | `h w`              | `B > 1` and `T > 1`                 |

Units `1` and `2` place one non-spatial axis around `h w`. That axis is `b` when
`B > 1`, and `t` when `T > 1`.

Units `0` and `9` differ only in the order the index walks the two axes at one
tile location: `b` then `t` for `0`, `t` then `b` for `9`. That order is
observable only when both `B` and `T` exceed `1`, which is why `9` is valid
nowhere else.

The diagram shows how the two frame types use the band and time axes. For a
tile, `b` and `t` select the frame. For a cell, they are part of the decoded
frame.

![frame_unit keeps band and time outside a tile frame or places them around h w for cell storage](img/rumi-frame-unit.svg)

Within a decoded frame, `h w` MUST stay together and in that order.

The registry is complete and append-only; existing values MUST NOT be
reassigned. A reader MUST reject any `frame_unit` value or `(B, T)` combination
not listed above. A valid `frame_unit` determines the decoded sample order and
the number of frames `N`; it does not otherwise change the file structure.

#### Choosing a frame unit

This section is informative. Units `0` and `9` provide the finest access: one
frame contains one tile for one band and one time step. Every other unit places
one cell in each frame, allowing OpenZL to model correlation between bands,
time steps, or both.

An axis before `h w` is stored as contiguous planes. An axis after `h w` is
interleaved within each pixel. When both axes precede `h w`, the axis next to
`h w` varies between adjacent planes.

The best unit depends on the expected reads and the data. Compression SHOULD be
measured when more than one unit fits the access pattern.

### Frame index

`row` and `col` select a tile location in the spatial grid. Tile locations are
traversed in row-major order.

```text
tiles_across = ceil(image_width / tile_width)
tiles_down   = ceil(image_length / tile_length)
g            = tiles_across * tiles_down
```

When a frame holds a cell, there is one frame per tile location.

```text
frame_index(row, col) = row * tiles_across + col
N                     = g
```

When a frame holds a tile, the index also walks the band and time axes.

```text
spatial = row * tiles_across + col

frame_unit 0:  frame_index = (spatial * B + b) * T + t
frame_unit 9:  frame_index = (spatial * T + t) * B + b

N = g * B * T
```

### Validating frame_unit

Tag `65000` stores `frame_unit` in the file. Its value MUST be registered and
valid for `B` and `T`.

The entry counts of `TileOffsets` and `TileByteCounts` MUST also match the frame
unit:

```text
count == g * B * T   when the frame holds a tile
count == g           when the frame holds a cell
```

A reader MUST reject a file when either count is wrong. The counts do not
distinguish `0` from `9`, or one full-frame layout from another; tag `65000`
does.

### Frame order

Frames MUST be stored in increasing `frame_index` order. `TileOffsets` and
`TileByteCounts` MUST use the same order.

This order allows the external header to reconstruct offsets with a prefix sum.
A reader MUST reject a file whose `TileOffsets` do not match the reconstructed
offsets in frame-index order.

## Sample encodings

`sample_format` gives the sample type and `bits_per_sample` its width in bits.
Unsigned integers use ordinary binary representation, and signed integers use
two's-complement representation. IEEE formats use the IEEE 754 binary16,
binary32, or binary64 encoding named in the table.

A complex sample stores two equal-width components: real first, then imaginary.
For complex formats, `bits_per_sample` is their combined width.

| sample_format | meaning                      |
| ------------- | ---------------------------- |
| `1`           | unsigned integer             |
| `2`           | signed integer               |
| `3`           | IEEE floating point          |
| `5`           | complex signed integer       |
| `6`           | complex IEEE floating point  |
| `100`..`106`  | rumi-private ML and EO types |

Only the following pairs are valid:

| sample_format | bits_per_sample | encoding                                       |
| ------------- | --------------- | ---------------------------------------------- |
| 1             | 1               | 1-bit binary                                   |
| 1             | 2               | unsigned 2-bit integer                         |
| 1             | 4               | unsigned 4-bit integer                         |
| 1             | 8               | unsigned 8-bit integer                         |
| 1             | 16              | unsigned 16-bit integer                        |
| 1             | 32              | unsigned 32-bit integer                        |
| 1             | 64              | unsigned 64-bit integer                        |
| 2             | 2               | signed 2-bit integer                           |
| 2             | 4               | signed 4-bit integer                           |
| 2             | 8               | signed 8-bit integer                           |
| 2             | 16              | signed 16-bit integer                          |
| 2             | 32              | signed 32-bit integer                          |
| 2             | 64              | signed 64-bit integer                          |
| 3             | 16              | IEEE 16-bit floating point                     |
| 3             | 32              | IEEE 32-bit floating point                     |
| 3             | 64              | IEEE 64-bit floating point                     |
| 5             | 32              | complex signed integer, 16-bit components      |
| 5             | 64              | complex signed integer, 32-bit components      |
| 6             | 32              | complex IEEE floating point, 16-bit components |
| 6             | 64              | complex IEEE floating point, 32-bit components |
| 6             | 128             | complex IEEE floating point, 64-bit components |
| 100           | 8               | float8 E4M3FN                                  |
| 101           | 8               | float8 E5M2                                    |
| 102           | 16              | bfloat16                                       |
| 103           | 8               | float8 E8M0FNU                                 |
| 104           | 6               | float6 E2M3FN                                  |
| 105           | 6               | float6 E3M2FN                                  |
| 106           | 4               | float4 E2M1FN                                  |

A reader MUST reject any pair not listed above.

`bfloat16` has one sign bit, eight exponent bits, and seven fraction bits, with
the exponent and special values of IEEE binary32. `E4M3FN` and `E5M2` use the
[ONNX float8 encodings](https://onnx.ai/onnx/technical/float8.html).
`E8M0FNU`, `E2M3FN`, `E3M2FN`, and `E2M1FN` use the corresponding encodings in
the [OCP Microscaling Formats (MX) Specification
1.0](https://www.opencompute.org/documents/ocp-microscaling-formats-mx-v1-0-spec-final-pdf).

All bands and time steps in a file MUST use the same pair.

`bits_per_sample` is the logical width of a sample, not necessarily its storage
stride. When `bits_per_sample` is less than `8`, decoded frames MUST use padded
storage: each sample occupies one byte, its encoding occupies the least
significant `bits_per_sample` bits, and the unused high bits MUST be zero. Signed
sub-byte integers use two's-complement representation in the occupied bits.
Packed sub-byte storage MUST NOT be used.

The decoded frame size is defined by:

```text
decoded_samples  = h * w           when the frame holds a tile
                   B * T * h * w   when the frame holds a cell

bytes_per_sample = 1                       if bits_per_sample < 8
                   bits_per_sample / 8     otherwise

decoded_frame_bytes = decoded_samples * bytes_per_sample
```

An absent axis contributes a factor of one. [Resource limits](#resource-limits)
applies to `decoded_frame_bytes`.

## Bit-packed arrays

Time residuals and frame byte-count residuals use the same bit packing.

Values are stored consecutively using `bits` bits each. Bit `j` of value `i`
occupies bit position `i * bits + j`, with bits numbered from the least
significant bit of each byte. For `n` values, the region is exactly
`ceil(n * bits / 8)` bytes. Unused bits in the final byte MUST be zero.

When `bits` is `0`, the region is empty and every value is zero.

## File profile

A file is rumi compliant when all of the following hold.

- It begins with the rumi file header and contains exactly one rumi IFD.
- It is tiled and has no overviews, masks, strips, or auxiliary IFDs.
- Its IFD precedes the frame data, and its tags, values, and frame placement
  follow [Fixed IFD](#fixed-ifd), without gaps or padding.
- Its sample encoding is listed in [Sample encodings](#sample-encodings).
- Its georeferencing follows [Georeferencing](#georeferencing).
- Each frame is a self-contained OpenZL frame.
- `FrameUnit`, `TileOffsets`, and `TileByteCounts` satisfy
  [Validating frame_unit](#validating-frame_unit).
- Every frame is present, every byte count is greater than zero, and the frames
  form one contiguous run in frame-index order.
- It ends with the trailer defined in [Time coordinates](#time-coordinates).

## File header

Every rumi file begins with this 16-byte header.

| offset | size | type   | name       |
| ------ | ---- | ------ | ---------- |
| 0      | 4    | bytes  | magic      |
| 4      | 2    | uint16 | version    |
| 6      | 2    | uint16 | reserved   |
| 8      | 8    | uint64 | ifd_offset |

The magic bytes spell ASCII `RUMI`: `52 55 4D 49`. The current version is `1`,
`reserved` is zero, and `ifd_offset` is `16`. A reader MUST reject any other
value.

The IFD uses the 20-byte entry layout and tag numbers derived from BigTIFF, but
rumi defines its own tags, placement, and alignment. A rumi file is not a TIFF,
BigTIFF, or GeoTIFF file.

## Fixed IFD

A rumi IFD MUST contain exactly the tags below, in rising tag order. A reader
that validates the file or builds an external header from it MUST reject any
other tag.

`B` is `samples_per_pixel`, `T` is `time_count`, and `N` is the frame count.

The IFD begins with the eight-byte entry count `13` and ends with an eight-byte
zero offset for the next IFD. Each entry has this layout:

| offset | size | type   | name            |
| ------ | ---- | ------ | --------------- |
| 0      | 2    | uint16 | tag             |
| 2      | 2    | uint16 | type            |
| 4      | 8    | uint64 | count           |
| 12     | 8    | bytes  | value or offset |

The type codes are `3` for `SHORT`, `4` for `LONG`, `12` for `DOUBLE`, and `16`
for `LONG8`. These represent `uint16`, `uint32`, IEEE 754 binary64, and `uint64`,
respectively.

| tag   | name                   | type   | count |
| ----- | ---------------------- | ------ | ----- |
| 256   | ImageWidth             | LONG   | 1     |
| 257   | ImageLength            | LONG   | 1     |
| 258   | BitsPerSample          | SHORT  | B     |
| 277   | SamplesPerPixel        | SHORT  | 1     |
| 322   | TileWidth              | SHORT  | 1     |
| 323   | TileLength             | SHORT  | 1     |
| 324   | TileOffsets            | LONG8  | N     |
| 325   | TileByteCounts         | LONG   | N     |
| 339   | SampleFormat           | SHORT  | B     |
| 34264 | ModelTransformationTag | DOUBLE | 16    |
| 34735 | GeoKeyDirectoryTag     | SHORT  | 16    |
| 65000 | FrameUnit              | SHORT  | 1     |
| 65001 | TimeCount              | LONG   | 1     |

Every file carries all 13 tags.

`ImageWidth`, `ImageLength`, `TimeCount`, `TileWidth`, `TileLength`, and
`SamplesPerPixel` MUST be greater than zero.

`BitsPerSample` and `SampleFormat` MUST contain `B` repetitions of one pair
listed in [Sample encodings](#sample-encodings).

### Placement

The IFD starts at byte `16`, immediately after the rumi file header. Its size is
`8 + 20 * 13 + 8 = 276` bytes: an eight-byte entry count, 13 entries, and an
eight-byte zero offset for the next IFD.

Values of 8 bytes or less MUST be stored in the IFD entry. Larger values MUST
follow the IFD in rising tag order, without gaps. Unused bytes in an inline
value MUST be zero. For an external value, the last eight bytes of the entry
store its `uint64` file offset.

The frame data starts immediately after the last external value. Padding or
alignment bytes MUST NOT be inserted in the external area.

### Deriving base_frame_offset

The IFD size is fixed. Only values larger than 8 bytes contribute to the
external area before the frames.

```text
external = (2 * B  if B >= 5 else 0)      # 258 BitsPerSample
         + (8 * N  if N >= 2 else 0)      # 324 TileOffsets
         + (4 * N  if N >= 3 else 0)      # 325 TileByteCounts
         + (2 * B  if B >= 5 else 0)      # 339 SampleFormat
         + 128                            # 34264 ModelTransformationTag
         + 32                             # 34735 GeoKeyDirectoryTag

base_frame_offset = 16 + 276 + external
                  = 292 + external
```

The result MUST match the first entry of `TileOffsets`. Files with the same `B`
and `N` start their frame data at the same byte.

## Georeferencing

Every rumi file carries `ModelTransformationTag` and `GeoKeyDirectoryTag`.
`ModelPixelScaleTag` (33550), `ModelTiepointTag` (33922),
`GeoDoubleParamsTag` (34736), and `GeoAsciiParamsTag` (34737) MUST NOT appear.
Files without georeferencing use the values in
[Undefined georeferencing](#undefined-georeferencing).

### ModelTransformationTag

`ModelTransformationTag` stores a 4 × 4 matrix as 16 doubles in row-major order.
North-up rasters set the rotation terms to zero.

Given affine coefficients
`(x_res, row_rot, x_origin, col_rot, y_res, y_origin)`, the matrix is

```text
x_res    row_rot  0  x_origin
col_rot  y_res    0  y_origin
0        0        0  0
0        0        0  1
```

The third row MUST be zero.

### GeoKeyDirectoryTag

rumi represents a CRS by EPSG code. `GeoKeyDirectoryTag` contains a four-short
header followed by three four-short keys, for a total of 16 shorts.

| key                                           | id           | value                               |
| --------------------------------------------- | ------------ | ----------------------------------- |
| GTModelTypeGeoKey                             | 1024         | `1` projected or `2` geographic     |
| GTRasterTypeGeoKey                            | 1025         | `1` PixelIsArea or `2` PixelIsPoint |
| GeographicTypeGeoKey or ProjectedCSTypeGeoKey | 2048 or 3072 | EPSG code                           |

The four-short header MUST be `(1, 1, 0, 3)`. Each key is stored as
`(key_id, 0, 1, value)`, in the order shown above.

A geographic CRS uses key `2048`; a projected CRS uses key `3072`. The key MUST
agree with `GTModelTypeGeoKey`. The EPSG code MUST be defined by the EPSG
registry and fall between `1024` and `32766`.

A writer MUST obtain the CRS type from the EPSG registry or a PROJ database. It
MUST NOT infer the type from the numeric code. A reader obtains the type from
`GTModelTypeGeoKey`.

### Undefined georeferencing

An ungeoreferenced file still carries both georeferencing tags.

`ModelTransformationTag` uses the following matrix.

```text
1  0  0  0
0  1  0  0
0  0  0  0
0  0  0  1
```

`GeoKeyDirectoryTag` uses the same header and key representation:

| key                  | id   | value      |
| -------------------- | ---- | ---------- |
| GTModelTypeGeoKey    | 1024 | `0`        |
| GTRasterTypeGeoKey   | 1025 | `1` or `2` |
| GeographicTypeGeoKey | 2048 | `0`        |

A reader MUST interpret `GTModelTypeGeoKey = 0` as no CRS and MUST ignore the
transformation matrix.

The two tags always occupy 160 bytes.

No other CRS representation is permitted. This excludes WKT, PROJ strings,
ESRI codes, user-defined CRS values, engineering, compound and vertical CRSs,
and coordinate epochs.

## Time coordinates

Every rumi file ends with one time trailer. It begins immediately after the last
frame, and the file ends immediately after it. The external header does not
contain time coordinates.

Let `C` be the number of coordinates stored in the trailer:

```text
C = 0       when time_type is 0 (undefined)
C = T       when time_type is 2 (instant)
C = 2 * T   when time_type is 1 (interval)
```

For instants, `time(i)` is the coordinate of time step `i`.

For intervals, time step `i` covers `[time(2i), time(2i + 1))`. Each step stores
its own start and end, so intervals may leave gaps.

### Trailer fields

The fixed part is 28 bytes. All multi-byte fields are little-endian.

```text
+---------------+----------------------------------+
| Trailer       | time_residuals[C]                |
+---------------+----------------------------------+
  28 bytes        ceil(C * time_bits / 8) bytes
```

| offset | size | type   | name       |
| ------ | ---- | ------ | ---------- |
| 0      | 4    | uint32 | magic      |
| 4      | 2    | uint16 | version    |
| 6      | 1    | uint8  | time_type  |
| 7      | 1    | uint8  | time_bits  |
| 8      | 8    | int64  | time_epoch |
| 16     | 8    | int64  | time_step  |
| 24     | 4    | uint32 | time_scale |

The trailer size MUST be exactly `28 + ceil(C * time_bits / 8)` bytes and MUST
contain no padding.

#### magic

The four bytes spell ASCII `TIME`: `54 49 4D 45`. Read as a little-endian
`uint32`, they equal `0x454D4954`. A reader MUST reject any other value.

#### version

The current trailer version is `1`. A reader that implements version `1` MUST
reject any other value.

#### time_type

| value | meaning                                            |
| ----- | -------------------------------------------------- |
| `0`   | undefined; the file carries no time coordinates    |
| `1`   | interval; each step is bounded by two coordinates  |
| `2`   | instant; the step is a point in time               |

A reader MUST reject any other value.

Instant coordinates MUST be non-decreasing.

Interval coordinates MUST satisfy:

```text
time(2i) < time(2i + 1)         for 0 <= i < T
time(2i + 1) <= time(2i + 2)    for 0 <= i < T - 1
```

Intervals may meet or leave gaps, but MUST NOT overlap.

#### time_epoch, time_step and time_scale

`time_scale` is the number of seconds represented by one coordinate unit. For
defined time, it MUST be `86400` when every instant or interval endpoint is an
exact whole-day offset from `1970-01-01T00:00:00Z`; otherwise it MUST be `1`. A
reader MUST reject any other value.

Coordinate `time(i)` is a signed offset of `time(i) * time_scale` seconds from
`1970-01-01T00:00:00Z`; negative values represent times before that epoch. rumi
follows POSIX time and does not represent leap seconds.

For defined time, `time_epoch` MUST equal `time(0)`. `time_step` is the slope of
the prediction line used to encode the remaining coordinates:

```text
time_step = 0                                      if C < 2
time_step = round((time(C-1) - time(0)) / (C-1))  otherwise
```

`round` chooses the nearest integer; exact halves round toward positive
infinity.

#### time_bits

The number of bits used to encode each residual, as defined in
[Time residuals](#time-residuals). It MUST be between `0` and `64`.

### Time residuals

After the 28-byte fixed part, the trailer stores the `C` coordinates as residuals
against a straight line through `time_epoch` and `time_step`.

An axis whose coordinates lie on this line requires no packed region. Otherwise,
the trailer stores their deviations from the line.

#### Encoding

For defined time, let `time(i)` be coordinate `i` in `time_scale` units.

```text
predicted(i) = time_epoch + i * time_step
residual(i)  = time(i) - predicted(i)
```

A residual MUST fit in `int64`. It is mapped to an unsigned integer with zigzag
encoding:

```text
zigzag(x) = 2 * x       if x >= 0
            -2 * x - 1  otherwise

packed(i) = zigzag(residual(i))
time_bits = bit_length(max(packed))
```

`bit_length(0)` is `0`.

A writer MUST use the minimum `time_bits` that represents the largest packed
residual. `residual(0)` is always zero.

A reader MUST reject a trailer unless `time_scale` satisfies the rule above and
the decoded coordinates reproduce the recorded `time_epoch`, `time_step`, and
minimum `time_bits`. Every decoded coordinate MUST fit in `int64` and satisfy
the ordering required by `time_type`.

#### Packing

The `C` packed values are stored as defined in
[Bit-packed arrays](#bit-packed-arrays), at `time_bits` bits each.

#### Decoding

```text
residual(i) = packed(i) >> 1              if packed(i) is even
              -((packed(i) >> 1) + 1)     otherwise
time(i)     = time_epoch + i * time_step + residual(i)
```

### Undefined time

When time is undefined, `C` is zero and the fields MUST have these values:

| field      | value |
| ---------- | ----- |
| time_type  | `0`   |
| time_bits  | `0`   |
| time_epoch | `0`   |
| time_step  | `0`   |
| time_scale | `1`   |

The trailer is exactly 28 bytes. A reader MUST NOT interpret `time_epoch` as a
coordinate.

An Image or Cube MAY use `time_type = 0`. Cube steps are then ordered but
unlabelled.

## Header blob

The header blob is a binary record stored outside the rumi file. It contains the
raster fields and frame byte counts needed to locate a frame without parsing the
file.

The blob consists of a fixed 32-byte header followed by packed frame byte counts.
All multi-byte fields are little-endian.

```text
+---------------+----------------------------------+
| Header        | frame_byte_counts[N]             |
+---------------+----------------------------------+
  32 bytes        ceil(N * count_bits / 8) bytes
```

`N` is derived as defined in [Frame index](#frame-index).

The blob size MUST be exactly `32 + ceil(N * count_bits / 8)` bytes and contain
no padding.

The blob stores no offsets. They are reconstructed as defined in
[Deriving base_frame_offset](#deriving-base_frame_offset) and
[Offset reconstruction](#offset-reconstruction).

### Header fields

| offset | size | type   | name              |
| ------ | ---- | ------ | ----------------- |
| 0      | 4    | uint32 | magic             |
| 4      | 2    | uint16 | version           |
| 6      | 4    | uint32 | image_width       |
| 10     | 4    | uint32 | image_length      |
| 14     | 4    | uint32 | time_count        |
| 18     | 2    | uint16 | tile_width        |
| 20     | 2    | uint16 | tile_length       |
| 22     | 2    | uint16 | samples_per_pixel |
| 24     | 1    | uint8  | bits_per_sample   |
| 25     | 1    | uint8  | sample_format     |
| 26     | 1    | uint8  | frame_unit        |
| 27     | 4    | uint32 | count_min         |
| 31     | 1    | uint8  | count_bits        |

#### magic

The magic value is `0x45564F4C`, represented on the wire as `4C 4F 56 45`. A
reader MUST reject any other value.

#### version

The current binary format version is `1`. A reader that implements version `1`
MUST reject any other value.

#### image_width, image_length and time_count

The raster dimensions. Width and length are in pixels and match `ImageWidth` and
`ImageLength`. `time_count` matches the value of `TimeCount`.

All three MUST be greater than zero. A `time_count` of `1` is an Image; anything
larger is a Cube.

#### tile_width and tile_length

The nominal tile dimensions in pixels. They match `TileWidth` and `TileLength`.

Both values MUST be greater than zero.

The grid size is defined in [Frame index](#frame-index).

Edge tiles are clipped to the image bounds and are not padded. A reader MUST
derive their dimensions from the image shape and grid position.

#### samples_per_pixel

The band count `B`. This matches `SamplesPerPixel` in the IFD and MUST be at least `1`.

#### bits_per_sample and sample_format

The sample type and width, as defined in
[Sample encodings](#sample-encodings).

#### frame_unit

The frame layout, as defined in [frame_unit](#frame_unit). Together with the
image shape it determines `N`. Tag `65000` carries the same value inside the
file.

#### count_min and count_bits

The frame byte count encoding, as defined in
[Frame byte counts](#frame-byte-counts). `count_bits` MUST be between `0` and
`32`.

### Frame byte counts

After the fixed header, the blob stores the compressed size of each frame in
frame-index order. Every count MUST be greater than zero. Each count is encoded
as a residual from the minimum count.

#### Encoding

Let `c[i]` be the byte count of frame `i`.

```text
count_min  = min(c)
count_bits = 0 if max(c) == count_min else bit_length(max(c) - count_min)
```

`bit_length(x)` is the minimum number of bits required to represent `x`.

A writer MUST use these values. A reader MUST reject a blob if `count_min` is
not the minimum decoded count or `count_bits` is not the minimum required width.

A file with one frame always has `count_bits = 0`.

#### Packing

The `N` residuals `c[i] - count_min` are stored as defined in
[Bit-packed arrays](#bit-packed-arrays), at `count_bits` bits each.

#### Decoding

```text
c[i] = count_min + residual[i]
```

Every reconstructed count MUST fit in `uint32`.

### Creating the header blob

A writer or header builder MUST create the blob from the finalized rumi file.
Every duplicated value MUST match:

| header blob | rumi IFD |
| ----------- | -------- |
| `image_width` | `ImageWidth` |
| `image_length` | `ImageLength` |
| `time_count` | `TimeCount` |
| `tile_width` | `TileWidth` |
| `tile_length` | `TileLength` |
| `samples_per_pixel` | `SamplesPerPixel` |
| `bits_per_sample` | every `BitsPerSample` value |
| `sample_format` | every `SampleFormat` value |
| `frame_unit` | `FrameUnit` |
| `frame_byte_counts[i]` | `TileByteCounts[i]` |

`frame_byte_counts` and `TileByteCounts` MUST each contain `N` entries. The
writer or header builder MUST NOT produce the blob if any comparison fails.

This check happens when the blob is created. A stateless reader can then treat
the blob as authoritative and does not need to read or compare the IFD,
`TileOffsets`, `TileByteCounts`, the time trailer, or the total file size before
reading a frame. How an application keeps a blob associated with its file is
outside the scope of this specification.

### Offset reconstruction

The blob stores no frame offsets. A reader derives `base_frame_offset` and
reconstructs the offsets in frame-index order.

```text
offset[0]     = base_frame_offset
offset[idx+1] = offset[idx] + frame_byte_counts[idx]
```

Every reconstructed offset MUST fit in `uint64`.

A reader passes `frame_byte_counts[idx]` bytes at `offset[idx]` to the OpenZL
decoder.

The byte just past the last frame is where the time trailer begins.

```text
trailer_offset = offset[N-1] + frame_byte_counts[N-1]
```

`trailer_offset` can be used to read the time coordinates. Reading a frame does
not require parsing the trailer.

The offset of frame `k` can also be expressed as

```text
offset[k] = base_frame_offset
          + k * count_min
          + sum(residual[0:k])
```

## Resource limits

A reader MUST check derived sizes before allocating memory or decoding a frame.
An operation that exceeds the reader's resource limits MUST fail before the
allocation or decode. This does not make the rumi file invalid.

## Changelog

- 0.1.0. Initial draft.
