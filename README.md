<p align="center">
  <img src="img/rumi-lockup-tight.svg" alt="rumi" width="750"/>
</p>

<p align="center">
  <a href="https://pypi.org/project/rumi-eo/"><img src="https://img.shields.io/pypi/v/rumi-eo.svg?color=2b8a3e" alt="PyPI"/></a>
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-blue" alt="Platform"/>
  <a href="#license"><img src="https://img.shields.io/badge/license-GPLv3-green.svg" alt="License: GPLv3"/></a>
</p>

<p align="center"><i>rumi is the Quechua word for stone.</i></p>

A GeoTIFF can be written in countless ways, and that flexibility is half of why they get painful to read at scale. Deep learning reads millions of chips per epoch, and with loose layouts a reader has to work out each file before it can touch the pixels.

rumi solves that by supporting only one layout. A rumi file is always **BigTIFF, tiled, band separate, and tile interleaved**. Every tile is a self-contained [OpenZL](https://github.com/facebook/openzl) frame. There are no overviews and nothing left to guess about. The full rules live in the [specification](SPEC.md).

Because the layout is fixed, almost everything about a rumi file is predictable. The metadata that is left fits in a tiny self-contained space, so a million files stay indexed in memory and reads go straight to the pixels.

<p align="center">
  <img src="img/rumi-index.svg" alt="rumi index" width="720"/>
</p>

## Status

rumi is **experimental**. The on-disk format and the header blob can change between versions with no migration path. Do not use it for archival data you cannot regenerate.

> [!WARNING]
> **TIFF compression tag 60000 is experimental, not a registered code.**
> rumi marks OpenZL-compressed tiles with TIFF `Compression` tag `60000`. This is not a registered TIFF compression code, it is a private-range value rumi uses by convention. A standard TIFF or GeoTIFF reader cannot decode a rumi file, it sees an unknown compression. The value may change if OpenZL is assigned an official tag or if it collides with another private use. A rumi file is only readable by a reader that knows `60000` means OpenZL.

## Install

```bash
pip install rumi-eo
```

## Quick start

```python
import numpy as np
import rumi

arr = np.random.randint(0, 4096, (4, 2048, 2048), dtype=np.uint16)  # (B, Y, X)

# write: rumi tiles, sweeps predictors per tile, keeps the smallest, assembles.
# returns the path and the header blob, the bytes you cache in a catalog.
path, header = rumi.write("scene.tif", arr)

# read: hand back a numpy array, nothing else needed
arr = rumi.read("scene.tif")                          # (B, Y, X)

# window a read, and reuse the cached blob to skip re-reading the header
arr = rumi.read("scene.tif", header, b=[0, 3], y=(0, 512), x=(0, 512))
```

## Write

```python
path, header = rumi.write("scene.tif", arr, method="2d-full",
                          chunk_size=512, max_error=None,
                          transform=None, crs=None)
```

`write` takes a `(B, Y, X)` array, cuts it into `chunk_size` tiles, and for each tile runs the method's predictors and keeps the smallest frame. It returns `(path, header)`, where `header` is a bytes blob you cache next to the path in your catalog, a Parquet column works well, and hand back to `read`.

- **`method`** picks the predictor set swept per tile. `"2d-full"` tries every predictor for the best ratio; `"2d-simd"` tries a smaller SIMD-friendly set for a faster write; `"2d-<name>"` forces a single predictor.
- **`chunk_size`** is the tile edge in pixels, a multiple of 16.
- **`max_error`** turns on near-lossless: an absolute error bound in DN, quantized ahead of the predictor. `None` is lossless.
- **`transform`** and **`crs`** carry georeferencing, given together. `transform` is a GDAL-style affine, `crs` an EPSG code or projection string.

```python
transform = (300000.0, 10.0, 0.0, 4000000.0, 0.0, -10.0)  # origin x, px w, 0, origin y, 0, -px h
path, header = rumi.write("scene.tif", arr, transform=transform, crs=32630)
```

## Read

```python
arr = rumi.read("scene.tif")                                     # whole image, (B, Y, X) numpy
arr = rumi.read("scene.tif", header)                             # reuse a cached header blob
arr = rumi.read("scene.tif", framework="torch")                  # torch tensor instead
arr = rumi.read("scene.tif", b=[3, 2, 1], y=(0, 512), x=(0, 512))
arr = rumi.read("scene.tif", num_threads=4)                      # parallel decode
```

Returns the image in the framework you pick. The second argument is the header blob; leave it out and rumi reads the header from the file, pass the cached blob to skip that. Each axis can be windowed.

- a tuple `(start, stop)` is a slice, the cheap case since it keeps tiles in disk order. `y` and `x` only take a slice or all.
- a list `[i, j, k]` picks those 0-based positions in that order. Fine for `b` (and `n` on a stack), more flexible but it can scatter the read.
- leaving an axis out reads all of it.

Prefer slices. rumi keeps all bands of a tile together, so a band slice reads them in order without stepping over the ones you skip. It is still one read per tile, so the win is locality and readahead, not a single seek.

**`framework`** selects the return type: `"numpy"` (default), `"torch"`, `"jax"`, `"tensorflow"`, or `None` for a zero-copy `RumiArray` you convert over DLPack. **`num_threads`** sets decode parallelism. The pool is process global and sized on first use, so the first threaded read fixes the count for the whole process. Default is single threaded.

## Stack

```python
arr = rumi.read(paths, headers, n=(0, 12), b=(0, 4))   # (12, 4, Y, X)
```

Pass lists of paths (and optionally their cached header blobs) and `read` adds an `n` axis over the assets. Window `n` like any other axis. The assets must match in size and encoding or it raises, no ragged cubes. This stacking is in memory at read time. For an N cube that lives on disk as one object, see the companion `zumi` format.

## Data model

<p align="center">
  <img src="img/rumi-data-model.svg" alt="rumi data model" width="720"/>
</p>

| level | shape | what it is |
|---|---|---|
| tile | T x T | one band at one grid position, one OpenZL frame |
| cell | B x T x T | every band at one position, contiguous on disk |
| Image | (B, Y, X) | one grid of cells, one rumi file |
| Cube | (N, B, Y, X) | N grid-aligned Images stacked |
| ImageCollection | set of Images | Images that do not share a grid |
| CubeCollection | set of Cubes | Cubes that do not share a grid |

One rumi file is one Image. An `ImageCollection` is just a set of rumi files, so it needs no format of its own. The `Cube` comes from the companion `zumi` format, and a `CubeCollection` is just a set of zumis.

The names come from stone. rumi is a single stone, one Image. zumi is the wall raised from them, the Cube.

## License

GPL-3.0

<div align="center">
  <br>
  Made with &#9829; by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="img/asterisk_banner.svg" alt="Asterisk Labs" width="400"/>
  </a>
</div>