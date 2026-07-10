import numpy as np

from . import _repr
from ._dtype import name as dtype_name, numpy_dtype
from ._ffi import PathLike, _blob_from_file, _check, ffi, lib


class RumiHeader:
    """Inspection view over a header blob. Wraps the C rumi_spec the blob parses
    into, plus the header struct read off it. Build from bytes in hand, or from
    a file with from_path. read() does not need this, it takes the blob."""

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

    @classmethod
    def from_path(cls, path: PathLike) -> "RumiHeader":
        """Read the header off disk and build. Tile data is not touched."""
        return cls(_blob_from_file(path))

    @property
    def shape(self) -> tuple[int, int, int]:
        h = self._header
        return (h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self) -> type[np.generic]:
        return numpy_dtype(self._header.dtype)

    def to_dict(self) -> dict:
        """Header fields as a plain, JSON-serializable dict, for a catalog or a
        Parquet column set. Pass straight to json.dumps if you want a string."""
        h = self._header
        return {
            "shape": list(self.shape),
            "bands": int(h.samples_per_pixel),
            "height": int(h.image_length),
            "width": int(h.image_width),
            "dtype": dtype_name(h.dtype),
            "tile": [int(h.tile_width), int(h.tile_length)],
            "tiles_across": int(h.tiles_across),
            "tiles_down": int(h.tiles_down),
            "tiles": int(h.tiles_across * h.tiles_down * h.samples_per_pixel),
            "base_tiles_offset": int(h.base_tiles_offset),
            "codec": "OpenZL",
        }

    def _facts(self) -> dict:
        # short keys the repr layer expects; ok=False lets it degrade gracefully
        try:
            h = self._header
            return {
                "ok": True,
                "b": h.samples_per_pixel, "y": h.image_length, "x": h.image_width,
                "dtype": dtype_name(h.dtype),
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