# rumi

- Specification 0.1.0
- Status Draft
- Date 2026-08-13
- License GPLv3

rumi is a profile for GeoTIFF files paired with a compact binary header that lets a reader locate every tile without parsing the TIFF IFD. Tile payloads are compressed with OpenZL.

A rumi header is predictable. The same tags, always, in the same order, with every value in the same place. Nothing in front of the tile data depends on the pixels or on the CRS. So if you know the shape of the image, you know which byte the tile data starts at. You do not have to open the file to find out.

The words MUST, MUST NOT, SHOULD, and MAY in this document carry the RFC 2119 meaning.

## Data model

rumi realizes part of the AI4EO Data Model, which organizes raster as a hierarchy of grid-aligned tensors built for training.

![Deep Learning Raster Data Model](img/rumi-data-model.svg)

| level | shape | definition |
|---|---|---|
| tile | T × T | one band at one grid position. The compressed unit, one OpenZL frame |
| cell | B × T × T | every band at one grid position, contiguous on disk |
| Image | (B, Y, X) | one grid of cells. One rumi file |
| Cube | (N, B, Y, X) | N grid-aligned Images stacked |
| ImageCollection | set of Images | Images that do not share a grid |
| CubeCollection | set of Cubes | Cubes that do not share a grid |

Grid-aligned Images stack into a single 4D tensor, the Cube. Without a shared grid they stay a list, a Collection.

rumi defines the bottom three levels, tile, cell, and Image. One rumi file is one Image. The zumi format defines Cube.

## Scope

This document defines the rumi file profile, the exact contents of the single IFD, and the binary layout of the rumi header blob. The profile defines which GeoTIFFs are valid rumi inputs. The blob lets a reader locate every compressed tile.

## File profile

A GeoTIFF is rumi compliant when all of the following hold.

- the file is BigTIFF
- the file uses little-endian byte order, so the TIFF header begins with `II`
- the file has exactly one IFD
- the file has no overviews
- the file has no masks or auxiliary IFDs
- the image is tiled, not stripped
- `PlanarConfiguration` is `2` (Separate)
- tiles are stored tile-interleaved
- the single IFD is positioned before the tile data
- the IFD holds exactly the tags listed in Fixed IFD, and no others
- out-of-line tag values are placed as defined in Fixed IFD, with no gap before the tile data
- georeferencing is encoded as defined in Georeferencing
- `Compression` is `60000` (OpenZL)
- `PhotometricInterpretation` is `1` (MinIsBlack)
- `Predictor` is `1`. rumi uses no TIFF predictor. All modelling lives inside the OpenZL frame
- `bits_per_sample` is `1`, `2`, `4`, `6`, `8`, `16`, `32`, `64`, or `128`
- `SampleFormat` is `1`, `2`, `3`, `5`, `6`, or a rumi-private value in `100`..`106`
- the `(sample_format, bits_per_sample)` pair is one listed in the sample encoding table
- each tile payload is one self-contained OpenZL frame, every frame in the file written at one pinned OpenZL frame format version
- `TileOffsets` and `TileByteCounts` are present
- `TileOffsets` and `TileByteCounts` have `tiles_across * tiles_down * samples_per_pixel` entries
- no tile is sparse. Every tile payload is present and every `TileByteCounts` entry is greater than zero
- the tile payloads form one contiguous run in tile-index order, each payload immediately followed by the next, with no framing between them

## Fixed IFD

A rumi IFD holds the tags below and nothing else, in rising tag order. A writer MUST NOT add tags. Not for the software that wrote the file, not for a description, not for nodata, not for statistics, not for anything.

Reading tiles never touches the IFD, so this rule falls on writers and on the readers that do parse it, the ones that rebuild a header blob from a file or check that a file is compliant. Such a reader MUST reject a file that carries any other tag.

`B` is `samples_per_pixel` and `N` is the tile count.

| tag | name | type | count |
|---|---|---|---|
| 256 | ImageWidth | LONG | 1 |
| 257 | ImageLength | LONG | 1 |
| 258 | BitsPerSample | SHORT | B |
| 259 | Compression | SHORT | 1 |
| 262 | PhotometricInterpretation | SHORT | 1 |
| 277 | SamplesPerPixel | SHORT | 1 |
| 284 | PlanarConfiguration | SHORT | 1 |
| 317 | Predictor | SHORT | 1 |
| 322 | TileWidth | SHORT | 1 |
| 323 | TileLength | SHORT | 1 |
| 324 | TileOffsets | LONG8 | N |
| 325 | TileByteCounts | LONG | N |
| 339 | SampleFormat | SHORT | B |
| 34264 | ModelTransformationTag | DOUBLE | 16 |
| 34735 | GeoKeyDirectoryTag | SHORT | 16 |

Every rumi file carries all 15 tags. A file that is not georeferenced still writes the last two, with the values given in Undefined georeferencing.

`PhotometricInterpretation` is `1`, MinIsBlack, in every rumi file. TIFF uses the tag to say how samples turn into colour, and nothing in a rumi file turns into colour. Bands are stored separately, there can be any number of them, and a sample can be a float8 or a complex pair, so no value is meaningful. It is written anyway because TIFF 6.0 requires it, and a rumi file should stay openable by any library that walks the IFD. `1` is the value that claims the least.

### Placement

The IFD starts at byte `16`, right after the BigTIFF header. It is always `8 + 20 * 15 + 8` bytes long, which is 316. That is the entry count, then the 15 entries, then the offset of the next IFD, which is zero because there is no next IFD.

A value of 8 bytes or less sits inside its own entry. A bigger value has to live somewhere else in the file. Those bigger values MUST start right after the IFD, MUST be written in rising tag order, and MUST have no gaps between them. Every value in the table has an even size, so none of them needs a pad byte, and a writer MUST NOT add one.

The tile data starts at the very next byte after the last of those values. A writer MUST NOT leave a gap anywhere in front of it. No padding, no alignment, no reserved space, no ghost area, not before the IFD and not between the values.

### Deriving base_tiles_offset

You can work out where the tile data starts from the shape of the image alone. The tag count never changes, so the only thing to add up is the values too big to sit inside their own entry, which are the ones over 8 bytes.

```text
external = (2 * B  if B >= 5 else 0)      # 258 BitsPerSample
         + (8 * N  if N >= 2 else 0)      # 324 TileOffsets
         + (4 * N  if N >= 3 else 0)      # 325 TileByteCounts
         + (2 * B  if B >= 5 else 0)      # 339 SampleFormat
         + 128                            # 34264 ModelTransformationTag
         + 32                             # 34735 GeoKeyDirectoryTag

base_tiles_offset = 16 + 316 + external
                  = 332 + external
```

The result MUST match the first entry of `TileOffsets`. Any two files of the same shape and band count start their tile data at the same byte.

## Georeferencing

Every rumi file carries `ModelTransformationTag` and `GeoKeyDirectoryTag`, and no other geo tag. `ModelPixelScaleTag` (33550), `ModelTiepointTag` (33922), `GeoDoubleParamsTag` (34736), and `GeoAsciiParamsTag` (34737) MUST NOT appear.

Neither tag is optional. A file with no georeferencing writes them both anyway, filled with the values in Undefined georeferencing. Georeferencing changes what the header says, never how long it is.

### ModelTransformationTag

The tag is the whole 4 by 4 matrix, 16 doubles, row by row. A north-up raster writes the same 16 doubles with the rotation terms set to zero. GeoTIFF would let it use the smaller scale and tiepoint pair instead, but then the size of the header would depend on how the raster happens to be rotated, and it must not.

Given affine coefficients `(x_res, row_rot, x_origin, col_rot, y_res, y_origin)` the matrix is the following.

```text
x_res    row_rot  0  x_origin
col_rot  y_res    0  y_origin
0        0        0  0
0        0        0  1
```

### GeoKeyDirectoryTag

The CRS is an EPSG code and nothing else. The directory is always 16 shorts, which is a header of four shorts saying there are three keys, then four shorts per key. That is 32 bytes in every rumi file.

| key | id | value |
|---|---|---|
| GTModelTypeGeoKey | 1024 | `1` projected, or `2` geographic |
| GTRasterTypeGeoKey | 1025 | `1` PixelIsArea, or `2` PixelIsPoint |
| GeographicTypeGeoKey or ProjectedCSTypeGeoKey | 2048 or 3072 | the EPSG code |

A geographic CRS uses key `2048` and a projected one uses key `3072`, matching what `GTModelTypeGeoKey` says. The code MUST be one the EPSG registry names, and it MUST fall between `1024` and `32766`.

### Undefined georeferencing

A raster does not always know where it is. A training chip cut from a scene, a synthetic tensor, a mask that lost its parent. Such a file still writes both tags, and says so in the values.

`ModelTransformationTag` is the identity matrix, which places the raster at the origin with a pixel one unit wide.

```text
1  0  0  0
0  1  0  0
0  0  0  0
0  0  0  1
```

`GeoKeyDirectoryTag` keeps its three keys and its 32 bytes. `GTModelTypeGeoKey` and key `2048` are both `0`, which GeoTIFF reads as intentionally omitted. `GTRasterTypeGeoKey` still says whether a pixel is an area or a point, since that is true of any raster.

| key | id | value |
|---|---|---|
| GTModelTypeGeoKey | 1024 | `0` |
| GTRasterTypeGeoKey | 1025 | `1` or `2` |
| GeographicTypeGeoKey | 2048 | `0` |

A reader MUST treat `GTModelTypeGeoKey` of `0` as no CRS, and MUST NOT read the identity matrix as a georeference. The 160 bytes go on the file either way. That is the price of a header whose length never moves, and it is a small one.

An ordinary GeoTIFF leaves the geo tags out instead, and each reader decides for itself what that means. GDAL fills in the identity matrix and an empty CRS, which is the same answer a rumi file gives outright, so nothing is lost in what gets reported. What is lost is the complaint. A reader that meets a file with no geo tags can say so, and a reader that meets an identity matrix has nothing to go on. So a rumi reader MUST report a file with `GTModelTypeGeoKey` of `0` as having no CRS, rather than passing on a georeference at the origin with pixels one unit wide.

If EPSG has no code for your CRS, it does not go in a rumi file. That rules out WKT, PROJ strings, ESRI codes above `32766`, the user-defined value `32767`, engineering CRS, compound and vertical CRS, and the coordinate epoch of a dynamic CRS. Each of them is as long as its contents make it, so the header would grow with the CRS and stop being predictable. A raster that needs one of them wants an ordinary GeoTIFF.

### Which codes are geographic

You cannot tell a geographic code from a projected one by looking at the number. They are not split into ranges, and codes that look geographic are sometimes projected. So a writer MUST look the code up at write time, in the EPSG registry or a PROJ database, and MUST NOT guess from the value. A reader takes the kind from `GTModelTypeGeoKey` and does not need the registry at all.

## Tile ordering

A tiled image with more than one sample can store its tiles in three orders.

| ordering | PlanarConfiguration | physical layout | rumi |
|---|---|---|---|
| pixel-interleaved | 1 (Contiguous) | one tile per spatial position holding all samples | not supported |
| band-interleaved | 2 (Separate) | every tile of sample 0, then every tile of sample 1 | not supported |
| tile-interleaved | 2 (Separate) | for each spatial position, the tiles of every sample together | required |

Pixel-interleaved is `PlanarConfiguration = 1`. rumi requires `PlanarConfiguration = 2`, so it is rejected by the planar-configuration rule.

Band-interleaved and tile-interleaved both report `PlanarConfiguration = 2`. The tag cannot tell them apart. They differ only in the physical order of tiles. rumi requires tile-interleaved and rejects band-interleaved.

The blob stores `tile_byte_counts` in tile-index order and reconstructs every offset by a single prefix sum. That reconstruction is correct only when the layout is samples innermost, because then tile-index order matches ascending file offset. A band-interleaved file orders the tiles differently, so prefix summing in tile-index order would point a reader at the wrong bytes.

For a single-sample image the three orderings coincide.

## Tile payloads

Each tile payload is one self-contained OpenZL frame. A writer MUST pin one OpenZL frame format version and write every frame at it, not at the latest available, so any reader built for that version can decode every tile. `rumi_openzl_format_version()` reports the version a rumi build targets.

A writer presents each tile to OpenZL as a typed numeric stream of the element type given by `sample_format` and `bits_per_sample`. OpenZL performs its own modelling and entropy coding and embeds the decode recipe in the frame. The OpenZL decoder reconstructs the tile from the frame, given the codecs the frame references. rumi stores no predictor and no codec metadata, because the frame carries its own decode recipe, including any custom codec parameters. This is why `Predictor` is fixed at `1`.

### Complex tiles

OpenZL has no complex element type, and its numeric streams are limited to element widths of 1, 2, 4, and 8 bytes, so a 16-byte complex sample cannot be presented as a single element. A complex sample is its real component type with twice the element count. A writer MUST present a complex tile to OpenZL as a numeric stream of the component type, signed integer or IEEE float of the component width, with `2 * T * T` elements. The components MAY be interleaved as a single stream in file order, real then imaginary, or deinterleaved into two streams, one per component. The frame carries which form was used. This mapping covers every complex encoding in the sample table without any complex-specific codec, the complex tile reuses the component-type code path.

## Header blob

The header blob is a small binary record stored outside the GeoTIFF and passed to the reader. A reader uses it directly and does not inspect the TIFF IFD before seeking to tile payloads.

The blob is a fixed-length header followed by the tile byte counts, packed at
the narrowest bit width that holds them.

```
+---------------+-----------------------------+
| Header        | tile_byte_counts[N]         |
+---------------+-----------------------------+
  27 bytes        ceil(N * count_bits / 8) bytes
```

The reader derives the tile grid and `N` from the header fields.

```text
tiles_across = ceil(image_width / tile_width)
tiles_down   = ceil(image_length / tile_length)
N            = tiles_across * tiles_down * samples_per_pixel
```

The full blob size MUST be exactly `27 + ceil(N * count_bits / 8)` bytes. The header is packed with no padding.

## Header fields

| offset | size | type | name |
|---|---|---|---|
| 0 | 4 | uint32 | magic |
| 4 | 2 | uint16 | version |
| 6 | 4 | uint32 | image_width |
| 10 | 4 | uint32 | image_length |
| 14 | 2 | uint16 | tile_width |
| 16 | 2 | uint16 | tile_length |
| 18 | 2 | uint16 | samples_per_pixel |
| 20 | 1 | uint8 | bits_per_sample |
| 21 | 1 | uint8 | sample_format |
| 22 | 4 | uint32 | count_min |
| 26 | 1 | uint8 | count_bits |

The header is 27 bytes.

The blob carries no tile offsets and no `base_tiles_offset`. Every one of them
follows from the fields above, so storing them would be storing an answer the
reader already has. Deriving base_tiles_offset gives the first, and Contiguity
and offset reconstruction gives the rest.

### magic

Identifies the blob as a rumi header. The value is `0x45564F4C`, which is the four bytes `4C 4F 56 45` on the wire, `LOVE`. A reader MUST reject any blob with a different value.

### version

The binary format version. The current value is `1`. A reader that implements only binary format 1 MUST reject any other version. Custom codecs do not change this version, because they add no field to the blob and carry their own parameters in the OpenZL codec header.

### image_width and image_length

The raster size in pixels. These match the TIFF tags `ImageWidth` and `ImageLength`.

### tile_width and tile_length

The tile size in pixels. These match the TIFF tags `TileWidth` and `TileLength`. TIFF 6.0 requires both to be a multiple of 16, so rumi requires it too. Each value MUST be a multiple of 16 and MUST be at least `16`. Tiles need not be square, and a tile dimension MAY exceed the corresponding image dimension, in which case the image is stored as a single tile cut to its own size.

Edge tiles are cut to where the image reaches, never padded, so a tile in the last column or the last row is smaller than `tile_width` by `tile_length`. Its size follows from the grid position and the image size, and a reader MUST derive it that way rather than trust a decoded length. The tile grid is always `ceil(image_width / tile_width)` by `ceil(image_length / tile_length)`, so a tile larger than the image yields a 1x1 grid per sample.

TIFF requires edge tiles to be padded, so a rumi file is not a valid TIFF. It was never readable as one anyway, since compression `60000` is not a TIFF codec.

### samples_per_pixel

The band count B. This matches the TIFF tag `SamplesPerPixel`. The value MUST be at least `1`. All samples use the same `bits_per_sample` and `sample_format`.

### bits_per_sample

The width in bits of one stored sample slot. For non-complex formats this is the per-sample bit width. For complex formats (`5`, `6`) this is the sum of the real and imaginary component widths. Valid values are `1`, `2`, `4`, `6`, `8`, `16`, `32`, `64`, and `128`. The sub-byte widths (`1`, `2`, `4`, `6`) belong to the packed integer, sub-byte float and binary types. A reader MUST reject any other value, and any `(sample_format, bits_per_sample)` pair not listed in the sample encoding table.

### sample_format

The sample type. `1` is unsigned integer. `2` is signed integer. `3` is IEEE floating point. `5` is complex signed integer. `6` is complex IEEE floating point. The values `100` through `106` are rumi-private formats for ML and EO types that standard TIFF has no `SampleFormat` for, they are never written by a plain TIFF encoder and only rumi produces them.

The valid sample encodings are listed below. This table and every projection of it, the enum, the byte size and the DLPack code, are generated from a single source, `core/include/rumi/rumi_dtypes.def`.

| sample_format | bits_per_sample | encoding |
|---|---|---|
| 1 | 1 | 1-bit binary |
| 1 | 2 | unsigned 2-bit integer |
| 1 | 4 | unsigned 4-bit integer |
| 1 | 8 | unsigned 8-bit integer |
| 1 | 16 | unsigned 16-bit integer |
| 1 | 32 | unsigned 32-bit integer |
| 1 | 64 | unsigned 64-bit integer |
| 2 | 2 | signed 2-bit integer |
| 2 | 4 | signed 4-bit integer |
| 2 | 8 | signed 8-bit integer |
| 2 | 16 | signed 16-bit integer |
| 2 | 32 | signed 32-bit integer |
| 2 | 64 | signed 64-bit integer |
| 3 | 16 | IEEE 16-bit floating point |
| 3 | 32 | IEEE 32-bit floating point |
| 3 | 64 | IEEE 64-bit floating point |
| 5 | 32 | complex signed integer, 16-bit real and 16-bit imaginary |
| 5 | 64 | complex signed integer, 32-bit real and 32-bit imaginary |
| 6 | 32 | complex IEEE floating point, 16-bit real and 16-bit imaginary |
| 6 | 64 | complex IEEE floating point, 32-bit real and 32-bit imaginary |
| 6 | 128 | complex IEEE floating point, 64-bit real and 64-bit imaginary |
| 100 | 8 | float8 E4M3FN |
| 101 | 8 | float8 E5M2 |
| 102 | 16 | bfloat16 |
| 103 | 8 | float8 E8M0 |
| 104 | 6 | float6 E2M3 |
| 105 | 6 | float6 E3M2 |
| 106 | 4 | float4 E2M1 |

A reader MUST reject any pair not listed above. This pair is also the element type a writer hands to OpenZL for the tile stream. For complex formats the writer hands OpenZL the component type, not the complex pair, as defined in Complex tiles.

### count_min and count_bits

The two parameters of the tile byte count encoding, defined in Tile byte counts.

`count_min` is the smallest byte count in the file. `count_bits` is the number of bits each stored count occupies, from `0` to `32`.

## Tile byte counts

After the 27-byte header, the blob stores the `N` compressed sizes of the tile OpenZL frames. Every count MUST be greater than zero.

Tiles are listed in tile-index order. All samples of one spatial tile are listed before the next spatial tile. Spatial positions are walked row-major.

The tile index for `(row, col, sample)` is `(row * tiles_across + col) * samples_per_pixel + sample`.

### Why the counts are not `uint32`

The header is 27 bytes. The counts are four bytes each, one per tile. On any real file they are almost the whole blob.

A `uint32` spends 32 bits on a number whether it needs them or not. The counts in one file are all close together, since tiles of the same shape compress to sizes of the same order, so the same high bits get written out `N` times.

rumi writes the smallest count once, then writes each count as the amount above it. Those amounts are small, and they are stored at the width the largest one needs.

The counts come back exactly. Nothing is rounded or approximated.

### Encoding

Let `c[i]` be the byte count of tile `i`.

```text
count_min  = min(c)
count_bits = 0 if max(c) == count_min else bit_length(max(c) - count_min)
```

`bit_length(x)` is the number of bits needed to write `x`, so `bit_length(1)` is `1` and `bit_length(255)` is `8`.

A writer MUST use exactly these two values. It MUST NOT widen `count_bits` beyond the minimum and MUST NOT lower `count_min` below the smallest count. The encoding is therefore canonical, and two writers given the same counts MUST produce the same bytes.

`count_bits` is `0` when every count is the same. Each residual is then zero, so nothing follows the header and every count is `count_min`. A file with a single tile always lands here.

### Packing

Residual `i` is `c[i] - count_min`. It is written at bit `i * count_bits`, low bit first. Residuals sit back to back with no gap, and none of them is aligned to anything. One that runs past the end of a byte continues in the next, low bits in the earlier byte.

The packed region is `ceil(N * count_bits / 8)` bytes. Spare bits in the last byte MUST be zero.

### Decoding

Residual `i` occupies the `count_bits` bits that start at bit `i * count_bits`, counting from the low bit of the first byte. Adding `count_min` to it gives the count.

```text
lo       = i * count_bits
residual = the count_bits bits at lo, read little endian
count    = count_min + residual
```

Every residual ends inside the packed region, so a reader never leaves the blob it was handed, and any tile can be read on its own.

A residual covers five bytes at most. Loading eight and shifting is simpler, and a reader may do that wherever eight bytes past the position are readable. In a blob held in a column they are not, and the last few tiles need a narrower read.

When `count_bits` is `0` there is nothing to read. Every count is `count_min`.

## Contiguity and offset reconstruction

rumi stores no offsets. A reader takes `base_tiles_offset` from the image shape, then adds up the counts in tile-index order to get the rest. A file is compliant only when the offsets it rebuilds match the file's `TileOffsets`, in the same order.

Walking `idx` from `0` upward, every tile payload offset MUST satisfy the following.

```text
offset[0]     = base_tiles_offset            # from Deriving base_tiles_offset
offset[idx+1] = offset[idx] + tile_byte_counts[idx]
```

Check them in tile-index order. Do not sort them first. A band-interleaved file also looks like one contiguous run once its offsets are sorted, but it breaks the rumi ordering rule, because the next payload on disk is not the next payload in tile-index order.

A reader takes exactly `tile_byte_counts[idx]` bytes from `offset[idx]` and hands them to the OpenZL decoder. The next payload starts on the very next byte.

Getting to the start of a window costs less than the walk suggests. Every count is `count_min` plus a residual, so the first `k` of them come to `k * count_min` plus the sum of the first `k` residuals. That sum is a plain total with nothing feeding forward, so it vectorises. Only the window itself has to be walked one tile at a time.

## Changelog

- 0.1.0. Initial draft.
