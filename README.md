<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="img/rumi-banner-dark.svg">
    <img src="img/rumi-banner-light.svg" alt="rumi" width="614">
  </picture>
  <p>
    <a href="https://github.com/asterisk-labs/rumi/actions/workflows/ci.yml"><img src="https://github.com/asterisk-labs/rumi/actions/workflows/ci.yml/badge.svg" alt="CI"/></a>
    <a href="https://pypi.org/project/rumi-eo/"><img src="https://img.shields.io/pypi/v/rumi-eo?color=2b8a3e&style=flat-square" alt="PyPI"/></a>
    <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-0078D6?style=flat-square" alt="Linux and macOS"/>
    <a href="#license"><img src="https://img.shields.io/badge/license-GPLv3-2b8a3e?style=flat-square" alt="GPLv3"/></a>
  </p>
  <p><i>rumi is the Quechua word for stone.</i></p>
</div>

---

Rumi is stateless raster storage for AI4EO. It cuts an Image or a Cube into
independently compressed [GeoZL](https://github.com/asterisk-labs/geozl) frames and
keeps their index in a tiny external header, so every read fetches only the frames it
needs.

## Features

- **The GDAL raster model, extended with time**, so one file carries an Image `(B, Y, X)`
  or a Cube `(T, B, Y, X)` along with its affine transform, CRS, band descriptions and
  dates.
- **Patterns in einops notation**, such as `b (row h) (col w) -> row col (b h w)` to store
  every band of a tile together, or `y x b` to read channels last.
- **Independent OpenZL frames**, normally compressed with GeoZL. Each frame can
  use a different graph; Python, R and Julia expose the linked GeoZL compressor.
- **Minibatches in one call**, with `read_many` fetching windows from many files
  concurrently, ideal for training data loaders.
- **Multithreaded decoding** that overlaps with downloads, set with
  `rumi.set_num_threads`.
- **Direct GPU reads** through CUDA on Linux and Metal on macOS, handed to
  PyTorch, JAX, CuPy or any other DLPack consumer. Rumi resolves the header and
  requested window on the CPU, then GeoZL writes the decoded samples into
  device memory that Rumi allocates.
- **Zero-copy arrays** for NumPy, PyTorch, JAX and TensorFlow, shared through DLPack,
  with 21 dtypes including bfloat16, float8 and complex.
- **Reads from anywhere** through [Karu](https://github.com/asterisk-labs/karu), whether
  the file sits on a local disk, behind HTTP, in S3, GCS, Azure, Hugging Face or Source
  Cooperative, or inside another file with `/vsisubfile/`.
- **A predictable header**, so `rumi.frame_start` knows where the frames begin before
  anything is compressed.
- **A C API** in `rumi.h`, so other languages can bind the same core.
- **A browser reader and MapLibre playground** for ranged URLs and local `.rumi` files.

## Bindings

| Language | Install | Role | Docs |
|----------|---------|------|------|
| Python | `pip install rumi-eo` | full API, GeoZL compression and GPU reads | [README](bindings/python/README.md) |
| R | `install.packages("rumi", repos = "https://asterisk-labs.r-universe.dev")` | read and write | [README](bindings/r/README.md) |
| Julia | `Pkg.Registry.add(url="https://github.com/asterisk-labs/AsteriskRegistry"); Pkg.add("Rumi")` | read and write | [README](bindings/julia/README.md) |
| JavaScript | `npm install @asterisk-labs/rumi` | read | [README](bindings/javascript/README.md) |

| Platform | Python package | GPU decode |
|----------|----------------|------------|
| Linux x86-64 | wheel on PyPI | NVIDIA, through CUDA |
| macOS arm64 | wheel on PyPI | Apple GPUs, through Metal |

The wheels need Python 3.11 or newer and also work without a GPU. The format may
change before 1.0. See the [guide](https://asterisk.coop/rumi/), the
[specification](SPEC.md), [compatibility](COMPATIBILITY.md) and the
[changelog](CHANGELOG.md).

## Quick start

```python
import numpy as np
import rumi

image = np.random.default_rng(0).integers(0, 4096, (4, 1024, 1024), dtype=np.uint16)
frames = rumi.frames(image, "b (row h) (col w) -> row col (b h w)", tile_size=512)
for frame in frames:
    graph = rumi.geozl.graph(frame.data, "planar>zigzag>zstd")
    frame.compressed = rumi.geozl.compress(frame.data, graph)

path = "scene.rumi"
header = rumi.write(path, frames, bands=["B2", "B3", "B4", "B8"], time=["2024-08-25"])
chip = rumi.read(path, header, bands=[2, 3], window=(0, 0, 256, 256))
```

## AI agent skill

Install the [Rumi skill](https://github.com/asterisk-labs/rumi/blob/main/.claude/skills/rumi/SKILL.md) so coding agents know its frame patterns, header rules and format internals.

```bash
npx skills add asterisk-labs/rumi
```

## Development

```bash
make submodules
make test       # Python and JavaScript suites
make help       # C++ tests, R, Julia, fuzzing and docs
```

## License

GPL-3.0. See [`LICENSE`](LICENSE).

<div align="center">
  <br>
  Made with ♥ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="https://raw.githubusercontent.com/asterisk-labs/cozip/refs/heads/main/images/asterisk_logo.svg" alt="Asterisk Labs" width="320"/>
  </a>
</div>
