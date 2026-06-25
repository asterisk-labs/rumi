# rumi

- Specification 0.1.0
- Status Draft
- Date 2026-06-25
- License GPLv3

rumi is a profile for GeoTIFF files paired with a compact binary header that lets a reader locate every tile without parsing the TIFF IFD. Tile payloads are compressed with OpenZL.

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

rumi defines the bottom three levels, tile, cell, and Image. One rumi file is one Image. The pirca format defines Cube.

## Scope

This document defines the rumi file profile and the binary layout of the rumi header blob. The profile defines which GeoTIFFs are valid rumi inputs. The blob lets a reader locate every compressed tile.

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
- `Compression` is `60000` (OpenZL)
- `Predictor` is `1`. rumi uses no TIFF predictor. All modelling lives inside the OpenZL frame
- `bits_per_sample` is `8`, `16`, `32`, `64`, or `128`
- `SampleFormat` is `1`, `2`, `3`, `5`, or `6`
- the `(sample_format, bits_per_sample)` pair is one listed in the sample encoding table
- each tile payload is one self-contained OpenZL frame at OpenZL frame format version `<OPENZL_FORMAT_VERSION>`
- `TileOffsets` and `TileByteCounts` are present
- `TileOffsets` and `TileByteCounts` have `tiles_across * tiles_down * samples_per_pixel` entries
- no tile is sparse. Every tile payload is present and every `TileByteCounts` entry is greater than zero
- the tile payloads form one contiguous run in tile-index order, each payload immediately followed by the next, with no framing between them

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

Each tile payload is one self-contained OpenZL frame written at OpenZL frame format version `<OPENZL_FORMAT_VERSION>`. A writer MUST pin this version, not the latest available, so any reader built for it can decode every tile.

A writer presents each tile to OpenZL as a typed numeric stream of the element type given by `sample_format` and `bits_per_sample`. OpenZL performs its own modelling and entropy coding and embeds the decode recipe in the frame. The OpenZL decoder reconstructs the tile from the frame, given the codecs the frame references. rumi stores no predictor and no codec metadata, because the frame carries its own decode recipe, including any custom codec parameters. This is why `Predictor` is fixed at `1`. Which codecs a frame may use, and the contract for lossless and lossy codecs, is defined in Codecs.

## Codecs

A tile frame is built from codecs. Two kinds exist.

Built-in OpenZL codecs need no registration. A frame that uses only built-in codecs decodes with any OpenZL reader.

rumi custom codecs do not ship with OpenZL. rumi registers them at runtime. They occupy a reserved custom transform id range, `0x72D700` to `0x72D7FF`. A frame that uses a rumi custom codec carries that codec's CTid, and the OpenZL decoder dispatches on the CTid alone. A reader MUST have the referenced custom codec registered to decode such a frame. 

rumi custom codecs fall in two namespaces, split by reconstruction contract and by sub-range so a reader can tell them apart from the CTid alone.

- `rumi.experimental.*`, range `0x72D700` to `0x72D77F`, are lossless. They satisfy `decode(encode(x)) = x`. A frame that uses only built-in and experimental codecs reconstructs the tile exactly.
- `rumi.lossy.*`, range `0x72D780` to `0x72D7FF`, are near lossless. They do not satisfy `decode(encode(x)) = x`. A frame that uses a lossy codec is not bit-exact, and the reconstruction error is bounded by the codec's parameters carried in the frame. A reader identifies a lossy frame from the head codec CTid without decoding the payload.

Custom codec parameters, such as a tile width or a quantization step, travel in the OpenZL codec header inside the frame. rumi therefore stores no codec metadata in the blob, and the binary format is unchanged by the use of custom codecs.

The currently assigned CTids are listed below.

| CTid | codec | namespace | reconstruction |
|---|---|---|---|
| 0x72D701 | delta_w | rumi.experimental | exact |
| 0x72D702 | delta_n | rumi.experimental | exact |
| 0x72D703 | planar | rumi.experimental | exact |
| 0x72D780 | quant_block | rumi.lossy | bounded per pixel error |

A reader that targets only built-in OpenZL is a valid rumi reader for files that use built-in codecs only. Decoding files that use rumi custom codecs requires a reader that registers them.

## Header blob

The header blob is a small binary record stored outside the GeoTIFF and passed to the reader. A reader uses it directly and does not inspect the TIFF IFD before seeking to tile payloads.

The blob is a fixed-length header followed by one `uint32` byte count per tile.

```
+---------------+---------------------------+
| Header        | tile_byte_counts[N]       |
+---------------+---------------------------+
  26 bytes        4 * N bytes
```

The reader derives the tile grid and `N` from the header fields.

```text
tiles_across = ceil(image_width / tile_width)
tiles_down   = ceil(image_length / tile_length)
N            = tiles_across * tiles_down * samples_per_pixel
```

The full blob size MUST be exactly `26 + 4 * N` bytes. The header is packed with no padding.

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
| 22 | 4 | uint32 | base_tiles_offset |

The header is 26 bytes.

### magic

Identifies the blob as a rumi header. The value is `0x333C333C`, the little-endian reading of the ASCII bytes `<`, `3`, `<`, `3`. A reader MUST reject any blob with a different value.

### version

The binary format version. The current value is `1`. A reader that implements only binary format 1 MUST reject any other version. Custom codecs do not change this version, because they add no field to the blob.

### image_width and image_length

The raster size in pixels. These match the TIFF tags `ImageWidth` and `ImageLength`.

### tile_width and tile_length

The tile size in pixels. These match the TIFF tags `TileWidth` and `TileLength`. Each value MUST be at least `1`. A tile dimension MAY exceed the corresponding image dimension. TIFF requires tile dimensions to be multiples of 16, so an image smaller than its tile size is stored as a single padded tile, and that file is compliant.

Tiles are uniform. Edge tiles are padded to the full tile size. The tile grid is always `ceil(image_width / tile_width)` by `ceil(image_length / tile_length)`, so a tile larger than the image yields a 1x1 grid per sample.

### samples_per_pixel

The band count B. This matches the TIFF tag `SamplesPerPixel`. The value MUST be at least `1`. All samples use the same `bits_per_sample` and `sample_format`.

### bits_per_sample

The width in bits of one stored sample slot. For non-complex formats (`1`, `2`, `3`) this is the per-sample bit width. For complex formats (`5`, `6`) this is the sum of the real and imaginary component widths. Valid values are `8`, `16`, `32`, `64`, and `128`. A reader MUST reject any other value, and any `(sample_format, bits_per_sample)` pair not listed in the sample encoding table.

### sample_format

The sample type. `1` is unsigned integer. `2` is signed integer. `3` is IEEE floating point. `5` is complex signed integer. `6` is complex IEEE floating point.

The valid sample encodings are listed below.

| sample_format | bits_per_sample | encoding |
|---|---|---|
| 1 | 8 | unsigned 8-bit integer |
| 1 | 16 | unsigned 16-bit integer |
| 1 | 32 | unsigned 32-bit integer |
| 1 | 64 | unsigned 64-bit integer |
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

A reader MUST reject any pair not listed above. This pair is also the element type a writer hands to OpenZL for the tile stream.

### base_tiles_offset

The absolute byte offset of the first compressed tile payload. It points to the first byte handed to the OpenZL decoder.

## Tile byte counts

After the 26-byte header, the blob stores `N` little-endian `uint32` values. Each is the compressed byte size of one tile OpenZL frame. Every value MUST be greater than zero.

Tiles are listed in tile-index order. All samples of one spatial tile are listed before the next spatial tile. Spatial positions are walked row-major.

The tile index for `(row, col, sample)` is `(row * tiles_across + col) * samples_per_pixel + sample`.

## Contiguity and offset reconstruction

rumi stores no explicit tile offsets. A reader reconstructs tile payload offsets by walking `tile_byte_counts` in tile-index order and prefix summing their values. A file is compliant only when the reconstructed offsets match the real TIFF `TileOffsets` in the same order.

Walking `idx` from `0` upward, every tile payload offset MUST satisfy the following.

```text
offset[0]     = base_tiles_offset
offset[idx+1] = offset[idx] + tile_byte_counts[idx]
```

The check MUST be performed in tile-index order. Do not sort offsets first. A band-interleaved file can still look like one contiguous run if offsets are sorted, but it fails the rumi ordering rule because the next payload in file order is not the next payload in tile-index order.

A reader reads exactly `tile_byte_counts[idx]` bytes from `offset[idx]` and hands them to the OpenZL decoder. The next payload begins immediately after, so the offset of the next tile is the current offset plus its byte count.

## Changelog

- 0.1.0. Initial draft.