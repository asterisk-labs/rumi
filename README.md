<p align="center">
  <img src="img/rumi-lockup-tight.svg" alt="rumi" width="750"/>
</p>

<p align="center">
  <a href="https://github.com/asterisk-labs/rumi/actions/workflows/ci.yml"><img src="https://github.com/asterisk-labs/rumi/actions/workflows/ci.yml/badge.svg" alt="CI"/></a>
  <a href="https://pypi.org/project/rumi-eo/"><img src="https://img.shields.io/pypi/v/rumi-eo.svg?color=2b8a3e" alt="PyPI"/></a>
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-blue" alt="Linux and macOS"/>
  <a href="#license"><img src="https://img.shields.io/badge/license-GPLv3-green.svg" alt="GPLv3"/></a>
</p>

<p align="center"><i>rumi is the Quechua word for stone.</i></p>

Rumi is an experimental raster format for machine-learning datasets. It stores
images as `(B, Y, X)` and time series as `(T, B, Y, X)`. Each frame can use a
different OpenZL compression graph, letting one file adapt compression to its
bands, times, or regions.

> [!WARNING]
> Rumi is not stable yet. The format and APIs may change before 1.0. It currently
> supports Linux and macOS only; Windows support depends on GeoZL supporting
> Windows.

## Install

```bash
pip install rumi-eo
```

Writing also needs GeoZL:

```bash
pip install "rumi-eo[write]"
```

Python 3.11 or newer is required.

## Quick start

```python
import geozl
import numpy as np
import rumi

image = np.random.default_rng(0).integers(
    0, 4096, size=(4, 1024, 1024), dtype=np.uint16
)

frames = rumi.frames(
    image,
    "b (row h) (col w) -> row col (b h w)",
    tile_size=512,
)

for frame in frames:
    graph = geozl.graph(frame.data, "planar>zigzag>zstd")
    frame.compressed = geozl.compress(frame.data, graph=graph)

path, header = rumi.write("scene.rumi", frames)

result = rumi.read(path, header)
chip = rumi.read(
    path,
    header,
    bands=[0, 3],
    window=(0, 0, 512, 512),
)
```

Selections are zero-based. A window is
`(row, column, height, width)`.

Use `read_many` when each source needs its own window:

```python
batch = rumi.read_many(
    paths,
    headers,
    windows=[(row, column, 256, 256) for row, column in positions],
    framework="torch",
)
```

## Cloud sources

Rumi reads remote files through its internal Karu transport. Both URI and GDAL
VSI spellings are accepted:

| Storage | URI | VSI path |
|---|---|---|
| Amazon S3 | `s3://bucket/key` | `/vsis3/bucket/key` |
| Google Cloud Storage | `gs://bucket/key` | `/vsigs/bucket/key` |
| Azure Blob Storage | `az://container/key` | `/vsiaz/container/key` |
| Azure Data Lake | `abfs://container/key` | `/vsiadls/container/key` |

Credentials and endpoints use the familiar GDAL/AWS/GCP/Azure environment
variables. For example:

```python
import os
import rumi

os.environ["AWS_PROFILE"] = "training"
os.environ["AWS_REGION"] = "us-west-2"

chip = rumi.read("s3://imagery/scene.rumi", header, window=(0, 0, 256, 256))
```

Common entry points are `GOOGLE_APPLICATION_CREDENTIALS` for GCS and
`AZURE_STORAGE_CONNECTION_STRING` (or the standard Azure identity variables)
for Azure. Anonymous access must be explicit with `AWS_NO_SIGN_REQUEST=YES`,
`GS_NO_SIGN_REQUEST=YES`, or `AZURE_NO_SIGN_REQUEST=YES`.

Each `read`, `read_many`, or `info(source=...)` operation snapshots the
environment into one private transport client. `read_many` shares that client
across all its sources so ranges can be batched, while no object bytes,
metadata, errors, or global client state survive the operation. Remote reads
require the external Rumi header; with it, Rumi does not issue a separate size
or existence request before fetching frame ranges.

## Metadata

`info` is the only metadata entry point:

```python
metadata = rumi.info(source="scene.rumi")
metadata = rumi.info(header=header)
metadata = rumi.info(source="scene.rumi", header=header)
```

Passing both validates that the external header matches the canonical index
reconstructed from the source. The check validates the index, not payload
identity.

## Documentation

- [Format specification](SPEC.md)
- [Compatibility policy](COMPATIBILITY.md)
- [Changelog](CHANGELOG.md)
- [Security policy](SECURITY.md)

## License

GPL-3.0
