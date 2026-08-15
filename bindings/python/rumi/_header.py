import numpy as np

from ._dtype import name as dtype_name
from ._dtype import numpy_dtype
from ._ffi import PathLike, _header_from_file, _Spec
from ._repr import header_html, header_text


class RumiHeader:
    """Parsed view of a binary rumi header."""

    def __init__(self, header: bytes | bytearray | memoryview) -> None:
        self._fields = _Spec(header).fields

    @classmethod
    def from_path(cls, path: PathLike) -> "RumiHeader":
        """Build a header view from a local rumi file."""
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
        """Return ``"tile"`` or ``"cell"``."""
        return "tile" if self._fields.frame_unit == 0 else "cell"

    @property
    def frames(self) -> int:
        """Return the total frame count."""
        h = self._fields
        per = h.samples_per_pixel if h.frame_unit == 0 else 1
        return int(h.tiles_across * h.tiles_down * per)

    def to_dict(self) -> dict:
        """Return JSON-serializable header metadata."""
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
            "base_frame_offset": int(h.base_frame_offset),
            "codec": "OpenZL",
        }

    def _facts(self) -> dict:
        # Compact keys used by the text and HTML representations.
        try:
            h = self._fields
            return {
                "ok": True,
                "b": h.samples_per_pixel, "y": h.image_length, "x": h.image_width,
                "dtype": dtype_name(h.dtype),
                "tile": (h.tile_width, h.tile_length),
                "across": h.tiles_across, "down": h.tiles_down,
                "unit": self.frame_unit,
                "frames": self.frames,
                "codec": "OpenZL",
            }
        except Exception:
            return {"ok": False}

    def __repr__(self) -> str:
        return header_text(self._facts())

    def _repr_html_(self) -> str:
        return header_html(self._facts())
