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

GPU reads decode straight into CUDA memory on Linux or Metal memory on macOS and
hand it to the framework through DLPack, so PyTorch, JAX and CuPy import it
without a copy.

```python
chip = rumi.read(path, header, framework="torch", device="cuda:0",
                 window=(row, column, height, width))

jax_chip = rumi.read(path, header, framework="jax", device="cuda:0",
                     window=(row, column, height, width))

cupy_chip = cupy.from_dlpack(rumi.read(
    path, header, framework="dlpack", device="cuda:0",
    window=(row, column, height, width)))

mps_chip = rumi.read(path, header, framework="torch", device="mps",
                     window=(row, column, height, width))

cpu_decoded = rumi.read(path, header, framework="torch", device="mps",
                         decode_backend="cpu",
                         window=(row, column, height, width))
```

The header, byte ranges and copy plan stay on the CPU. PFOR and `lanes` frames of
one- and two-byte samples decode on the device when they have one chunk; other
frames use CPU staging before GeoZL writes the result to the selected GPU.
`decode_backend="cpu"` forces complete CPU decoding for all selected frames;
`"auto"` is the default. A CUDA import makes the framework's current stream
wait for the read on the device, and the memory returns to Rumi in that stream's
order, so keep the stream alive while the array lives. MPS reads require PyTorch
2.10 or newer. GPU frame planning follows `rumi.set_num_threads` or
`RUMI_NUM_THREADS`, set before the first read, and defaults to one thread per
process. R and Julia device arrays are not implemented yet.
