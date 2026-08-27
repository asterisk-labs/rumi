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
- **Time coordinates.** Stores instants or interval boundaries in a compact
  trailer when the time axis is labelled.
- **Canonical structure.** Restricts the file to one fixed IFD, one frame order,
  and contiguous frame data.
- **Fixed-size georeferencing.** Stores an EPSG CRS and affine transform in a
  160-byte GeoTIFF block, with a defined value for ungeoreferenced rasters.

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

Unless a section says otherwise, all integer arithmetic used to validate or
derive sizes, counts, and offsets is exact. A reader MUST reject an input when a
required result cannot be represented by its implementation.

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

`time_count` is the number of time steps in a Cube. Their coordinates are
defined in [Time coordinates](#time-coordinates).

Collections are represented outside the file, for example by a catalogue of
rumi files. Their representation is out of scope.

## Frames

A frame is rumi's unit of compression and random access. Each frame is a
self-contained OpenZL frame that carries its own graph and codec parameters;
neither is stored in the IFD or external header. Its decoded shape and sample
order are specified by [`frame_unit`](#frame_unit).

`tile` and `cell` belong to the logical raster model. A `frame` is the physical
unit of compression and storage. A frame MUST contain exactly one of:

- one tile for one `(b, t)` pair at one tile location; or
- one cell at one tile location.

![Tiles and cells are data model units; a frame is their compressed storage unit](img/rumi-model-to-frame.svg)

### frame_unit

`frame_unit` selects one of the decoded layouts below. `b`, `t`, `h`, and `w`
mean band, time, height, and width. The rightmost axis changes fastest.

| frame_unit | decoded frame | frame order at each tile location |
| ---------- | ------------- | --------------------------------- |
| `0`        | `h w`         | `b`, then `t`                    |
| `1`        | `b h w`       | —                                |
| `2`        | `h w b`       | —                                |
| `3`        | `b t h w`     | —                                |
| `4`        | `t b h w`     | —                                |
| `5`        | `b h w t`     | —                                |
| `6`        | `t h w b`     | —                                |
| `7`        | `h w b t`     | —                                |
| `8`        | `h w t b`     | —                                |
| `9`        | `h w`         | `t`, then `b`                    |
| `10`       | `t h w`       | —                                |
| `11`       | `h w t`       | —                                |

The diagram shows how the two frame types use the band and time axes. For a
tile, `b` and `t` select the frame. For a cell, they are part of the decoded
frame.

![frame_unit keeps band and time outside a tile frame or places them around h w for cell storage](img/rumi-frame-unit.svg)

Within a decoded frame, `h w` MUST stay together and in that order.

The registry is complete and append-only; existing values MUST NOT be
reassigned. Not every registered value applies to every raster:

| values | condition |
| ------ | --------- |
| `0`    | any `B` and `T`; required instead of `9` unless both exceed `1` |
| `1`, `2` | `B > 1` and `T = 1` |
| `3` through `8` | `B > 1` and `T > 1` |
| `9` | `B > 1` and `T > 1` |
| `10`, `11` | `B = 1` and `T > 1` |

A reader MUST reject any `frame_unit` value or `(B, T)` combination not covered
above. A valid `frame_unit` determines the decoded sample order and the number
of frames `N`; it does not otherwise change the file structure.

#### Choosing a frame unit

This section is informative. Units `0` and `9` provide the finest access: one
frame contains one tile for one band and one time step. Every other unit places
one cell in each frame, allowing OpenZL to model correlation between bands,
time steps, or both.

An axis before `h w` is stored as contiguous planes. An axis after `h w` is
interleaved within each pixel. When both axes precede `h w`, the axis next to
`h w` varies between adjacent planes.

| frame_unit | frame layout | use when |
| ---------- | ------------ | -------- |
| `0` | `h w` | independent band/time reads; time steps adjacent within each band |
| `1` | `b h w` | planar bands in a multi-band Image |
| `2` | `h w b` | pixel-interleaved bands in a multi-band Image |
| `3` | `b t h w` | planar Cube; temporal correlation within each band |
| `4` | `t b h w` | planar Cube; band correlation within each time step |
| `5` | `b h w t` | planar bands; time steps interleaved per pixel |
| `6` | `t h w b` | planar time steps; bands interleaved per pixel |
| `7` | `h w b t` | both axes interleaved per pixel; time varies fastest |
| `8` | `h w t b` | both axes interleaved per pixel; band varies fastest |
| `9` | `h w` | independent band/time reads; bands adjacent within each time step |
| `10` | `t h w` | planar time steps in a single-band Cube |
| `11` | `h w t` | time steps interleaved per pixel in a single-band Cube |

These are selection guidelines, not conformance requirements. Compression
depends on the data and SHOULD be measured when more than one unit fits the
access pattern.

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

These formulas also apply when `B` or `T` is `1`.

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

For units `0` and `9`, all band and time-step frames at one tile location come
before any frame at the next tile location. A file that groups tile locations
by band or time instead is invalid.

This order allows the external header to reconstruct offsets with a prefix sum.
A reader MUST reject a file whose `TileOffsets` do not match the reconstructed
offsets in frame-index order.

## Sample encodings

`sample_format` gives the sample type and `bits_per_sample` its width in bits.
For complex formats, `bits_per_sample` is the combined width of the real and
imaginary components.

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
| 103           | 8               | float8 E8M0                                    |
| 104           | 6               | float6 E2M3                                    |
| 105           | 6               | float6 E3M2                                    |
| 106           | 4               | float4 E2M1                                    |

A reader MUST reject any pair not listed above.

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

An absent axis contributes a factor of one. A reader MUST compute
`decoded_frame_bytes` with exact arithmetic and check it against its resource
limit before allocating or decoding the frame.

## File profile

A file is rumi compliant when all of the following hold.

- It is a little-endian BigTIFF file with exactly one IFD.
- It is tiled and has no overviews, masks, strips, or auxiliary IFDs.
- Its IFD appears before the frame data and contains exactly the tags listed in
  [Fixed IFD](#fixed-ifd).
- Its tag values and frame data use the placement defined in
  [Fixed IFD](#fixed-ifd), without gaps or padding.
- Its sample encoding is listed in [Sample encodings](#sample-encodings).
- Its georeferencing follows [Georeferencing](#georeferencing).
- Each frame is a self-contained OpenZL frame.
- `FrameUnit`, `TileOffsets`, and `TileByteCounts` satisfy
  [Validating frame_unit](#validating-frame_unit).
- Every frame is present, every byte count is greater than zero, and the frames
  form one contiguous run in frame-index order.
- The time trailer begins immediately after the last frame, and the file ends
  immediately after the trailer.

## Fixed IFD

A rumi IFD MUST contain exactly the tags below, in rising tag order. A reader
that validates the file or builds an external header from it MUST reject any
other tag.

`B` is `samples_per_pixel`, `T` is `time_count`, and `N` is the frame count.

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

`TimeCount` stores `T`, the number of time steps in the raster.

`ImageWidth`, `ImageLength`, `TimeCount`, `TileWidth`, `TileLength`, and
`SamplesPerPixel` MUST be greater than zero.

`BitsPerSample` and `SampleFormat` MUST contain `B` repetitions of one pair
listed in [Sample encodings](#sample-encodings).

### Placement

The IFD starts at byte `16`, immediately after the BigTIFF header. Its size is
`8 + 20 * 13 + 8 = 276` bytes: an eight-byte entry count, 13 entries, and an
eight-byte zero offset for the next IFD.

Values of 8 bytes or less MUST be stored in the IFD entry. Larger values MUST
follow the IFD in rising tag order, without gaps.

The frame data starts immediately after the last external value. Padding,
alignment, and reserved bytes MUST NOT appear before it.

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

A rumi file ends with a time trailer. The trailer begins immediately after the
last frame, and the file ends immediately after the trailer. Every file carries
one.

The external header does not contain time coordinates.

Let `C` be the number of coordinates stored in the trailer:

```text
C = 0       when time_type is 0 (undefined)
C = T       when time_type is 2 (instant)
C = T + 1   when time_type is 1 (interval)
```

For instants, `time(i)` is the coordinate of time step `i`. For intervals,
time step `i` covers `[time(i), time(i+1))`. Storing `T + 1` boundaries makes
the final interval explicit.

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

Instant coordinates MUST be non-decreasing. Interval boundaries MUST be
strictly increasing.

#### time_epoch, time_step and time_scale

`time_scale` is the number of seconds represented by one coordinate unit. It
MUST be greater than zero. A writer SHOULD use the largest whole-second unit
that represents every coordinate exactly.

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

For this encoding, a regular axis is an arithmetic progression in integer
`time_scale` units. A periodic axis with non-constant integer spacing still
requires residuals.

#### time_bits

The number of bits used to encode each residual, as defined in
[Time residuals](#time-residuals). It MUST be between `0` and `64`.

### Time residuals

After the 28-byte fixed part, the trailer stores the `C` coordinates as residuals
against a straight line through `time_epoch` and `time_step`.

A regular axis has zero residuals and requires no packed region. An irregular
axis stores only its deviation from the prediction line.

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

A reader MUST reject a trailer whose decoded coordinates do not reproduce the
recorded `time_epoch`, `time_step`, and minimum `time_bits`. Every decoded
coordinate MUST fit in `int64` and satisfy the ordering required by `time_type`.

#### Packing

Residuals are packed consecutively using `time_bits` bits each. Bit `j` of
`packed(i)` is stored at bit position `i * time_bits + j`, with bits numbered
from the least significant bit of each byte. The packed region is exactly
`ceil(C * time_bits / 8)` bytes. Unused bits in the final byte MUST be zero.

#### Decoding

```text
lo          = i * time_bits
packed(i)   = the time_bits bits starting at lo
residual(i) = packed(i) >> 1              if packed(i) is even
              -((packed(i) >> 1) + 1)     otherwise
time(i)     = time_epoch + i * time_step + residual(i)
```

When `time_bits` is `0`, every residual is zero and the axis is exactly the line.

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

The frame layout, as defined in [frame_unit](#frame_unit). A reader MUST reject
an unknown value or one that is invalid for `B` and `T`.

Together with the image shape, `frame_unit` determines `N`. Tag `65000` carries
the same value inside the file.

#### count_min and count_bits

`count_min` and `count_bits` define the encoding in
[Frame byte counts](#frame-byte-counts).

`count_min` is the smallest frame byte count. `count_bits` is the residual width
and MUST be between `0` and `32`.

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

When all counts are equal, `count_bits` is `0` and no packed data follows the
header. Every count is then `count_min`. A file with one frame always has
`count_bits = 0`.

#### Packing

Residual `i` is

```text
residual[i] = c[i] - count_min
```

Residuals are packed consecutively using `count_bits` bits each. Bit `j` of
`residual[i]` is stored at bit position `i * count_bits + j`, with bits numbered
from the least significant bit of each byte.

The packed region is exactly `ceil(N * count_bits / 8)` bytes. Unused bits in
the final byte MUST be zero.

#### Decoding

```text
lo          = i * count_bits
residual[i] = the count_bits bits starting at lo
c[i]        = count_min + residual[i]
```

When `count_bits` is `0`, every count is `count_min`.

Every reconstructed count MUST fit in `uint32`.

### Matching the rumi file

A header blob is valid for a rumi file only when every duplicated value
matches:

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

`frame_byte_counts` and `TileByteCounts` MUST each contain `N` entries. A reader
MUST reject the pair if any comparison fails.
[Offset reconstruction](#offset-reconstruction) defines the corresponding
check against `TileOffsets`.

### Offset reconstruction

The blob stores no frame offsets. A reader derives `base_frame_offset` and
reconstructs the offsets in frame-index order.

```text
offset[0]     = base_frame_offset
offset[idx+1] = offset[idx] + frame_byte_counts[idx]
```

Every reconstructed offset MUST fit in `uint64`.

A reader MUST compare the reconstructed offsets with `TileOffsets` in
frame-index order, without sorting either sequence.

A reader passes `frame_byte_counts[idx]` bytes at `offset[idx]` to the OpenZL
decoder.

The byte just past the last frame is where the time trailer begins.

```text
trailer_offset = offset[N-1] + frame_byte_counts[N-1]
```

A reader MUST parse the trailer at `trailer_offset` and reject the file unless
its size is exactly `trailer_offset + trailer_size`.

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
