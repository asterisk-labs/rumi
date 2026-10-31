# rumi (python)

Python bindings for Rumi, stateless raster storage for AI4EO. Rumi stores images as
`(B, Y, X)` and time series as `(T, B, Y, X)`.

```bash
pip install rumi-eo
```

Rumi currently supports Linux and macOS and requires Python 3.11 or newer. The
format and APIs may change before 1.0. See the
[project README](https://github.com/asterisk-labs/rumi) for examples and the
[format specification](https://github.com/asterisk-labs/rumi/blob/main/SPEC.md)
for the wire format.

Reads return NumPy by default. PyTorch, JAX and TensorFlow results use DLPack;
`framework="dlpack"` returns the one-shot producer itself. Compatibility is
checked before opening the source, and Rumi never casts a dtype to make it fit.

On Linux, PyTorch can read straight into a CUDA tensor:

```python
chip = rumi.read(path, header, framework="torch", device="cuda:0",
                 window=(row, column, height, width))
```

The header, byte ranges and copy plan stay on the CPU. Decoded samples remain on
the selected GPU. R and Julia device arrays are not implemented yet.
