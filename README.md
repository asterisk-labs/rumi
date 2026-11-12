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

Rumi is stateless raster storage for AI4EO. It cuts an Image or a Cube into independently
compressed frames and keeps their index in a tiny external header, so every read fetches
only the frames it needs.

## Features

- **The GDAL raster model, extended with time**, so one file carries an Image `(B, Y, X)`
  or a Cube `(T, B, Y, X)` along with its affine transform, CRS, band descriptions and
  dates.
- **The tile `(H, W)` is the smallest unit**, and bands and dates are ordered around it,
  either one per frame or all together in one frame.
- **Patterns in einops notation**, such as `b (row h) (col w) -> row col (b h w)` to store
  every band of a tile together, or `y x b` to read channels last.
- **GeoZL is the only supported codec**, lossless or lossy, and each frame can use a
  different compression graph.
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

## Quick start

```python
import geozl
import numpy as np
import rumi

image = np.random.default_rng(0).integers(0, 4096, (4, 1024, 1024), dtype=np.uint16)
frames = rumi.frames(image, "b (row h) (col w) -> row col (b h w)", tile_size=512)
for frame in frames:
    graph = geozl.graph(frame.data, "planar>zigzag>zstd")
    frame.compressed = geozl.compress(frame.data, graph=graph)

path, header = rumi.write("scene.rumi", frames, bands=["B2", "B3", "B4", "B8"],
                          time=["2024-08-25"])
with rumi.open(path, header) as file:
    chip = file.read(bands=[2, 3], window=(0, 0, 256, 256))
```

The wheels can place the same partial read directly on a GPU:

```python
gpu_chip = rumi.read(path, header, bands=[2, 3],
                     window=(0, 0, 256, 256),
                     framework="torch", device="cuda:0")

# Any DLPack framework, here JAX and CuPy:
jax_chip = rumi.read(path, header, window=(0, 0, 256, 256),
                     framework="jax", device="cuda:0")
cupy_chip = cupy.from_dlpack(rumi.read(path, header, window=(0, 0, 256, 256),
                                       framework="dlpack", device="cuda:0"))

# On Apple Silicon:
mps_chip = rumi.read(path, header, bands=[2, 3],
                     window=(0, 0, 256, 256),
                     framework="torch", device="mps")

# Decode every selected frame on the CPU, then place samples on the GPU:
cpu_decoded_chip = rumi.read(path, header, bands=[2, 3],
                             window=(0, 0, 256, 256),
                             framework="torch", device="mps",
                             decode_backend="cpu")
```

The default `decode_backend="auto"` uses the GPU for supported single-chunk
frames and the CPU for other frames. A frame is never split between decoders.
A CUDA import makes the framework's current stream wait for the read on the
device, without blocking the host. MPS reads require PyTorch 2.10 or newer.

GPU frame planning uses the same process-wide count as CPU reads. Set it before
the first read with `rumi.set_num_threads(n)` or `RUMI_NUM_THREADS`; each DDP
rank has its own setting, which defaults to one.

[Run the CUDA example in Colab](https://colab.research.google.com/github/asterisk-labs/rumi/blob/main/examples/rumi-gpu.ipynb).

## Installation

```bash
pip install rumi-eo
npm install @asterisk-labs/rumi
```

The R and Julia readers live in `bindings/r` and `bindings/julia`. Python, R and
Julia can open a source with its external header. JavaScript opens the complete
`.rumi` file and reads only the required byte ranges. R and Julia use one-based
indices and native `(X, Y, Band, Time)` arrays; Python and JavaScript use
zero-based indices and `(Time, Band, Y, X)`.

Wheels are available for Linux x86-64 and macOS arm64 on Python 3.11 or newer.
Linux wheels include CUDA decoding and macOS wheels include Metal decoding;
both still work without an available GPU. The format may change before 1.0.

## Documentation

[Guide](https://asterisk.coop/rumi/) · [Specification](SPEC.md) ·
[Compatibility](COMPATIBILITY.md) · [Changelog](CHANGELOG.md) · [Security](SECURITY.md)

Coding agents can install the Rumi skill with `npx skills add asterisk-labs/rumi`.

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
