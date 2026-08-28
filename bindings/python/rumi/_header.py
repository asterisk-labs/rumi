import numpy as np

from ._dtype import name as dtype_name
from ._dtype import numpy_dtype
from ._ffi import PathLike, _header_from_file, _Spec
from ._pattern import frame_count, index_axes, layout_name
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
    def shape(self) -> tuple[int, ...]:
        """An Image is (bands, height, width); a Cube leads with time."""
        h = self._fields
        spatial = (int(h.samples_per_pixel), int(h.image_length),
                   int(h.image_width))
        return (int(h.time_count), *spatial) if h.time_count > 1 else spatial

    @property
    def time_count(self) -> int:
        return int(self._fields.time_count)

    @property
    def dtype(self) -> type[np.generic]:
        return numpy_dtype(self._fields.dtype)

    @property
    def frame_unit(self) -> str:
        """Return the frame's axis order, such as ``"b h w"``."""
        h = self._fields
        return layout_name(h.frame_unit, h.samples_per_pixel, h.time_count)

    @property
    def index_order(self) -> tuple[str, ...]:
        """Return the axes the frame index walks, outermost first."""
        h = self._fields
        return index_axes(h.frame_unit, h.samples_per_pixel, h.time_count)

    @property
    def frames(self) -> int:
        """Return the total frame count."""
        h = self._fields
        return frame_count(h.frame_unit, h.image_width, h.image_length,
                           h.tile_width, h.samples_per_pixel, h.time_count)[2]

    def to_dict(self) -> dict:
        """Return JSON-serializable header metadata."""
        h = self._fields
        return {
            "shape": list(self.shape),
            "bands": int(h.samples_per_pixel),
            "time_count": int(h.time_count),
            "height": int(h.image_length),
            "width": int(h.image_width),
            "dtype": dtype_name(h.dtype),
            "tile": [int(h.tile_width), int(h.tile_length)],
            "tiles_across": int(h.tiles_across),
            "tiles_down": int(h.tiles_down),
            "frame_unit": self.frame_unit,
            "index_order": list(self.index_order),
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
                "b": h.samples_per_pixel, "y": h.image_length,
                "x": h.image_width, "steps": h.time_count,
                "dtype": dtype_name(h.dtype),
                "tile": (h.tile_width, h.tile_length),
                "across": h.tiles_across, "down": h.tiles_down,
                "layout": self.frame_unit,
                "per": self.frames // (h.tiles_across * h.tiles_down),
                "tiled": bool(self.index_order),
                "frames": self.frames,
                "codec": "OpenZL",
            }
        except Exception:
            return {"ok": False}

    def __repr__(self) -> str:
        return header_text(self._facts())

    def _repr_html_(self) -> str:
        return header_html(self._facts())
