<p align="center">
  <img src="img/rumi-lockup-tight.svg" alt="rumi" width="750"/>
</p>

<p align="center">
  <a href="https://pypi.org/project/rumi-eo/"><img src="https://img.shields.io/pypi/v/rumi-eo.svg?color=2b8a3e" alt="PyPI"/></a>
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-blue" alt="Platform"/>
  <a href="#license"><img src="https://img.shields.io/badge/license-GPLv3-green.svg" alt="License: GPLv3"/></a>
</p>

<p align="center"><i>rumi is the Quechua word for stone.</i></p>

rumi is a GeoTIFF-inspired raster format for machine-learning datasets. It stores an image `(B, Y, X)` or a time series `(T, B, Y, X)` in one file, and reads complete rasters or small windows directly into NumPy, PyTorch, JAX, and TensorFlow.

rumi stores each image as independently compressed [OpenZL](https://github.com/facebook/openzl) frames. A read decodes only the frames it needs.

rumi files use the `.rumi` extension. The exact binary layout is defined in the [format specification](SPEC.md).

<p align="center">
  <img src="img/rumi-index.svg" alt="rumi index" width="720"/>
</p>
<p align="center"><sub>rumi has one canonical structure. That simplicity makes it easier to optimize.</sub></p>

## Install

To read rumi files:

```bash
pip install rumi-eo
```

To write them as well, install the OpenZL writer dependency:

```bash
pip install "rumi-eo[write]"
```

Requires Python 3.11+. Wheels are available for Linux x86-64 and macOS arm64.

## Write and read an image

Writing has three steps: split the array into frames, compress each frame with `geozl`, and write the file.

The split is written as a pattern. It names the axes of your array, cuts the spatial ones into a grid, and says what one frame holds.

```python
import geozl
import numpy as np
import rumi

image = np.random.default_rng(0).integers(
    0, 4096, size=(4, 1024, 1024), dtype=np.uint16
)

# 1. Split the image into frames.
frames = rumi.frames(image, "b (row h) (col w) -> row col (b h w)", tile_size=512)

# 2. Compress every frame.
for frame in frames:
    graph = geozl.graph(frame.data, "planar>zigzag>zstd")
    frame.compressed = geozl.compress(frame.data, graph=graph)

# 3. Write the file. Keep the returned header for fast later reads.
path, header = rumi.write("scene.rumi", frames)

# Read the complete image as NumPy.
result = rumi.read(path, header)

# Read bands 0 and 3 from a 512 x 512 window.
chip = rumi.read(path, header, bands=[0, 3], window=(0, 0, 512, 512))
```

Selections are zero-based. A window is `(row, column, height, width)`.

## Frame layouts

The trailing group of the pattern is the frame, and its axis order decides what `geozl` can model and what a read can reach without touching the rest.

```python
"b (row h) (col w) -> row col (b h w)"   # every band, band planar
"b (row h) (col w) -> row col (h w b)"   # every band, the pixel's spectrum contiguous
"b (row h) (col w) -> row col b (h w)"   # one band per frame
```

The last layout can produce tile-aligned training samples without opening the
file first:

```python
chunks = rumi.chunks(
    header, tiles=(2, 2), time=1, bands=[0, 1, 2], edge="drop"
)
sample = rumi.read(path, header, **chunks[i])
```

`tiles=1` means one tile per sample. `edge="clip"` keeps smaller border
samples. Other frame layouts are rejected so a chunk never decodes a larger
cell and discards part of it.

Only `b` and `t` are reserved, so the names a split introduces are yours. The left side names your array, so an input in `(rows, columns, bands)` order needs no transpose first:

```python
frames = rumi.frames(image, "(row h) (col w) b -> row col (b h w)", tile_size=512)
```

Unlike einops, the split does not require the image to divide evenly. Edge frames are simply smaller.

## Time series

A `t` axis makes the file a cube. Nothing else about the API changes: the pattern names one more axis, and where you place it decides what the compressor may model.

```python
cube = np.random.default_rng(0).integers(
    0, 4096, size=(6, 4, 512, 512), dtype=np.uint16
)

# One frame per tile holding every band and step, time varying between planes.
frames = rumi.frames(cube, "t b (row h) (col w) -> row col (b t h w)", tile_size=256)
for frame in frames:
    graph = geozl.graph(frame.data, "planar>zigzag>zstd")
    frame.compressed = geozl.compress(frame.data, graph=graph)

path, header = rumi.write("series.rumi", frames, time=[
    "2024-05-01", "2024-06-01", "2024-07-01",
    "2024-08-01", "2024-09-01", "2024-10-01",
])

series = rumi.read(path, header)              # (6, 4, 512, 512)
summer = rumi.read(path, header, time=[2, 3], bands=[0])
when = rumi.read_time(path)                   # Time(steps=[date(2024, 5, 1), ...],
                                              #      kind='instant')
where = rumi.read_geo(path)                   # Geo(None, None, False): this one
                                              # was written without a CRS
```

## Georeferencing

Pass an affine transform and an EPSG code together, or neither.

```python
transform = (10.0, 0.0, 300000.0, 0.0, -10.0, 8100000.0)
path, header = rumi.write("utm.rumi", frames, transform=transform, crs=32718)
```

The transform is `(x_res, row_rot, x_origin, col_rot, y_res, y_origin)`. `crs` takes an `int`, an `"EPSG:32718"` string, or any object with a `to_epsg()`. Pass `pixel_is_point=True` to anchor a pixel at its centre rather than its top-left corner.

The header is a small binary index. Store it next to the file path in Parquet or another catalog and pass both values to `rumi.read`. If you omit it, rumi can rebuild it from a local file:

```python
result = rumi.read("scene.rumi")
```

The file names its own frame layout, so a rebuilt header reads the same samples as the one `write` returned. Keeping the header only saves the parse.

## PyTorch

Return a tensor by selecting the framework:

```python
tensor = rumi.read(path, header, framework="torch")
```

rumi uses one thread by default. Keep that default inside a `torch.utils.data.DataLoader`: its workers are already separate processes, so additional pools usually oversubscribe the CPU.

For an interactive full-image read in a single process, enable parallel decoding before the first parallel read:

```python
rumi.set_num_threads(8)
image = rumi.read(path, header)
```

The same setting is available as `RUMI_NUM_THREADS=8` or `RUMI_NUM_THREADS=ALL_CPUS`. Forked workers default to one rumi thread. If the environment sets a larger value, call `rumi.set_num_threads(1)` in `worker_init_fn` before the first read.

## Current limits

- rumi is beta software. Version 0.17 is its current compatibility baseline.
- A CRS must be an EPSG code, or be omitted.
- Sources are local paths or bytes already in memory; object-storage URLs are not read directly yet.

> [!NOTE]
> Create files with rumi's writer, available as `rumi.write` in Python and
> `rumi_write` in C. Independent readers are supported; independent writers are
> not. See the [compatibility policy](COMPATIBILITY.md) for details.

## Learn more

- [Format specification](SPEC.md)
- [Changelog](CHANGELOG.md)
- [Compatibility policy](COMPATIBILITY.md)
- [Design notes](WHATWELEARN.md)
- [Security policy](SECURITY.md)
- [Ten-minute notebook](examples/rumi-demo.ipynb)
- [Issue tracker](https://github.com/asterisk-labs/rumi/issues)

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
