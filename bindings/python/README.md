# rumi (python)

Python bindings for Rumi, an experimental raster format for machine-learning
datasets. Rumi stores images as `(B, Y, X)` and time series as `(T, B, Y, X)`.

```bash
pip install rumi-eo
```

Writing also requires GeoZL:

```bash
pip install "rumi-eo[write]"
```

Rumi currently supports Linux and macOS and requires Python 3.11 or newer. The
format and APIs may change before 1.0. See the
[project README](https://github.com/asterisk-labs/rumi) for examples and the
[format specification](https://github.com/asterisk-labs/rumi/blob/main/SPEC.md)
for the wire format.
