import numpy as np

from . import _repr
from ._dtype import np_dtype
from ._ffi import _check, ffi, lib


class Spec:
    """Parsed header blob. Pure memory, no file handle, reusable across reads."""

    def __init__(self, blob: bytes | bytearray | memoryview) -> None:
        if not isinstance(blob, (bytes, bytearray, memoryview)):
            raise TypeError(f"blob must be bytes-like, got {type(blob).__name__}")

        blob_buf = ffi.from_buffer("unsigned char[]", blob)
        handle_out = ffi.new("rumi_spec**")
        _check(lib.rumi_spec_parse(blob_buf, len(blob), handle_out))
        # ffi.gc frees the handle whenever it goes away, even mid-__init__.
        self._handle = ffi.gc(handle_out[0], lib.rumi_spec_destroy)

        header = ffi.new("rumi_header*")
        _check(lib.rumi_spec_header(self._handle, header))
        self._header = header
        self._dtype = np_dtype(header.sample_format, header.bits_per_sample)

    @property
    def shape(self) -> tuple[int, int, int]:
        h = self._header
        return (h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self) -> type[np.generic]:
        return self._dtype

    def _facts(self) -> dict:
        try:
            h = self._header
            return {
                "ok": True,
                "b": h.samples_per_pixel, "y": h.image_length, "x": h.image_width,
                "dtype": self._dtype.__name__,
                "tile": (h.tile_width, h.tile_length),
                "across": h.tiles_across, "down": h.tiles_down,
                "tiles": h.tiles_across * h.tiles_down * h.samples_per_pixel,
                "codec": "OpenZL",
            }
        except Exception:
            return {"ok": False}

    def __repr__(self) -> str:
        return _repr.text(self._facts())

    def _repr_html_(self) -> str:
        return _repr.html_(self._facts())