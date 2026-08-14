import numpy as np

from ._dtype import name as dtype_name, numpy_dtype
from ._ffi import PathLike, _Spec, _header_from_file
from ._repr import header_html, header_text


class RumiHeader:
    """Inspection view over a rumi header. The header is bytes, C parses it.
    Build from bytes in hand, or from a file with from_path. read() does not
    need this, it takes the bytes."""

    def __init__(self, header: bytes | bytearray | memoryview) -> None:
        self._fields = _Spec(header).fields

    @classmethod
    def from_path(cls, path: PathLike) -> "RumiHeader":
        """Read the header off disk and build. Frame data is not touched."""
        return cls(_header_from_file(path))

    @property
    def shape(self) -> tuple[int, int, int]:
        h = self._fields
        return (h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self) -> type[np.generic]:
        return numpy_dtype(self._fields.dtype)

    @property
    def frame_unit(self) -> str:
        """What one frame holds, "tile" for one band at one grid position and
        "cell" for every band at one."""
        return "tile" if self._fields.frame_unit == 0 else "cell"

    @property
    def frames(self) -> int:
        """Frame count, which follows from the grid and the frame unit."""
        h = self._fields
        per = h.samples_per_pixel if h.frame_unit == 0 else 1
        return int(h.tiles_across * h.tiles_down * per)

    def to_dict(self) -> dict:
        """Header fields as a plain, JSON-serializable dict, for a catalog or a
        Parquet column set. Pass straight to json.dumps if you want a string."""
        h = self._fields
        return {
            "shape": list(self.shape),
            "bands": int(h.samples_per_pixel),
            "height": int(h.image_length),
            "width": int(h.image_width),
            "dtype": dtype_name(h.dtype),
            "tile": [int(h.tile_width), int(h.tile_length)],
            "tiles_across": int(h.tiles_across),
            "tiles_down": int(h.tiles_down),
            "frame_unit": self.frame_unit,
            "frames": self.frames,
            "base_tiles_offset": int(h.base_tiles_offset),
            "codec": "OpenZL",
        }

    def _facts(self) -> dict:
        # short keys the repr layer expects; ok=False lets it degrade gracefully
        try:
            h = self._fields
            return {
                "ok": True,
                "b": h.samples_per_pixel, "y": h.image_length, "x": h.image_width,
                "dtype": dtype_name(h.dtype),
                "tile": (h.tile_width, h.tile_length),
                "across": h.tiles_across, "down": h.tiles_down,
                "unit": self.frame_unit,
                "tiles": self.frames,
                "codec": "OpenZL",
            }
        except Exception:
            return {"ok": False}

    def __repr__(self) -> str:
        return header_text(self._facts())

    def _repr_html_(self) -> str:
        return header_html(self._facts())
