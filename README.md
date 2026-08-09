<p align="center">
  <img src="img/rumi-lockup-tight.svg" alt="rumi" width="750"/>
</p>

<p align="center">
  <a href="https://pypi.org/project/rumi-eo/"><img src="https://img.shields.io/pypi/v/rumi-eo.svg?color=2b8a3e" alt="PyPI"/></a>
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-blue" alt="Platform"/>
  <a href="#license"><img src="https://img.shields.io/badge/license-GPLv3-green.svg" alt="License: GPLv3"/></a>
</p>

<p align="center"><i>rumi is the Quechua word for stone.</i></p>

A GeoTIFF can be written in countless ways, and that flexibility is half of why they get painful to read at scale. Striped or tiled, bands interleaved or planar, which codec, which predictor, offsets of 4 bytes or 8, how many IFDs. Each one is a question the reader answers before it can read anything, once per file, a million times.

rumi answers them the same way every time. A rumi GeoTIFF file is ALWAYS a **BigTIFF, tiled, band separate, and tile interleaved**, one IFD, no overviews, and every tile is a self-contained [OpenZL](https://github.com/facebook/openzl) frame. The full rules live in the [specification](SPEC.md).

Because the layout is fixed, almost everything about a rumi file is predictable. The metadata that is left fits in a tiny self-contained space, so a million files stay indexed in memory and reads go straight to the pixels.

<p align="center">
  <img src="img/rumi-index.svg" alt="rumi index" width="720"/>
</p>

<p align="center">
  <i>A GeoTIFF gives you thousands of valid ways to write the same array. rumi gives you just one way.</i>
</p>


## Status

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
import geozl
import rumi

arr = np.random.randint(0, 4096, (4, 2048, 2048), dtype=np.uint16)  # (B, Y, X)

# 1. Cut the Image into a TileFrame
tf = rumi.tile(arr, tile_size=512)

# 2. You build the geozl graph, run the graph on the tile, and compress the tile.
for t in tf:
    g = geozl.graph(t.data, "planar>zigzag>transpose>entropy")
    t.compressed = geozl.compress(t.data, graph=g)

# 3. Write the TileFrame to a rumi file, and cache the rumi header for later reads
path, header = rumi.write("scene.tif", tf)

# Hand back a numpy array
arr = rumi.read("scene.tif")                          # (B, Y, X)

# Window a read, and reuse the cached blob to skip re-reading the header
arr = rumi.read("scene.tif", header, b=[0, 3], y=(0, 512), x=(0, 512))
```

## Catalog

`rumi.write` returns two things, the file and its header. The file is an ordinary `.tif` that lives wherever your scenes already live. The header is a small binary record you keep yourself.

<p align="center">
  <img src="img/rumi-catalog.svg" alt="rumi.write returns a .tif and a header, and the headers become rows in a parquet catalog" width="720"/>
</p>

<p align="center">
  <i>One VSI path plus one header is one row, so a collection of files is a table.</i>
</p>

**The header is a bypass.** A normal reader has to scan the file before it can touch a pixel. Open it, parse the IFD, load the offset arrays, then seek. With the header in hand there is nothing to discover. The reader computes the byte range and goes straight to the data. Reads become stateless, no open, no parse, nothing cached per file.

**It is also far smaller.** A BigTIFF IFD spends 16 bytes per tile on `TileOffsets` and `TileByteCounts`, plus the tag block itself. rumi stores 4 bytes per tile and nothing else, because the layout is fixed and everything else is predictable.

So it is a row. A VSI path in one column, the header bytes in the other. That pair is everything you need to open a scene, so a million scenes is a million rows and nothing else. The collection stops being a pile of files and becomes one object you can query, where reaching any tile in it costs a single seek.

```python
import pyarrow as pa
import pyarrow.parquet as pq

rows = []
for scene, tf in scenes:
    path, header = rumi.write(scene, tf)
    rows.append({"vsi_path": path, "rumi_header": header})

pq.write_table(pa.Table.from_pylist(rows), "catalog.parquet")
```

A read then takes the row and goes straight to the pixels.

```python
row = pq.read_table("catalog.parquet").to_pylist()[0]

chip = rumi.read(row["vsi_path"], row["rumi_header"], y=(0, 512), x=(0, 512))
```

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