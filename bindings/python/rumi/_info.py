from dataclasses import dataclass

import numpy as np

from ._dtype import numpy_dtype
from ._ffi import PathLike, _check, _Source, ffi, lib
from ._pattern import index_axes, layout_name
from ._time import INSTANT, INTERVAL, _from_seconds

Header = bytes | bytearray | memoryview
InfoSource = PathLike | bytes | bytearray | memoryview


@dataclass(repr=False)
class Metadata:
    """Metadata needed to understand and read one rumi source."""

    header: bytes
    shape: tuple[int, ...]
    time_count: int
    dtype: type[np.generic]
    tile: tuple[int, int]
    frame_layout: str
    index_order: tuple[str, ...]
    frames: int
    time: list | None
    time_kind: str | None
    transform: tuple[float, ...] | None
    crs: int | None
    pixel_is_point: bool | None

    def __repr__(self) -> str:
        dtype = np.dtype(self.dtype).name
        return (f"<rumi.Metadata shape={self.shape} dtype={dtype} "
                f"tile={self.tile} frame_layout={self.frame_layout!r}>")


def _native_info(source=None, header: Header | None = None):
    if source is None and header is None:
        raise ValueError("info needs source, header, or both")
    if header is not None and not isinstance(header, (bytes, bytearray,
                                                        memoryview)):
        raise TypeError(
            f"header must be bytes-like, got {type(header).__name__}")

    src = _Source(source) if source is not None else None
    header_buf = (ffi.from_buffer("unsigned char[]", header)
                  if header is not None else ffi.NULL)
    result = ffi.new("rumi_metadata*")
    _check(lib.rumi_info(
        src.handle if src is not None else ffi.NULL,
        header_buf, len(header) if header is not None else 0, result))
    return result


def info(*, source: InfoSource | None = None,
         header: Header | None = None) -> Metadata:
    """Inspect a source or header, validating their match when both are given.

    Source metadata includes georeferencing and time. An external header alone
    contains only the fields needed for reading, so its ``time``, ``transform``
    and ``pixel_is_point`` values are ``None``. ``shape`` follows ``(B, Y, X)``
    or ``(T, B, Y, X)``, and ``tile`` is ``(height, width)``. When ``source``
    is given, ``Metadata.header`` contains its canonical external header.
    """
    result = _native_info(source=source, header=header)
    try:
        h = result.fields
        shape: tuple[int, ...] = (
            int(h.samples_per_pixel), int(h.image_length), int(h.image_width))
        if h.time_count > 1:
            shape = (int(h.time_count), *shape)

        has_source = bool(result.has_source)
        steps = None
        kind = None
        if has_source:
            coords = [int(result.time[i]) for i in range(result.time_coords)]
            if result.time_type == INSTANT:
                steps = [_from_seconds(value) for value in coords]
                kind = "instant"
            elif result.time_type == INTERVAL:
                steps = [(_from_seconds(coords[i]), _from_seconds(coords[i + 1]))
                         for i in range(0, len(coords), 2)]
                kind = "interval"
            else:
                steps = []

        crs = (int(result.epsg) or None) if has_source else None
        transform = None
        if has_source and crs is not None:
            transform = tuple(float(value) for value in result.transform)

        index_order = index_axes(
            h.frame_unit, h.samples_per_pixel, h.time_count)
        frames = int(h.tiles_across) * int(h.tiles_down)
        if "b" in index_order:
            frames *= int(h.samples_per_pixel)
        if "t" in index_order:
            frames *= int(h.time_count)

        return Metadata(
            header=bytes(ffi.buffer(result.blob, result.blob_size)),
            shape=shape,
            time_count=int(h.time_count),
            dtype=numpy_dtype(h.dtype),
            tile=(int(h.tile_length), int(h.tile_width)),
            frame_layout=layout_name(
                h.frame_unit, h.samples_per_pixel, h.time_count),
            index_order=index_order,
            frames=frames,
            time=steps,
            time_kind=kind,
            transform=transform,
            crs=crs,
            pixel_is_point=(bool(result.pixel_is_point)
                            if has_source else None),
        )
    finally:
        lib.rumi_metadata_free(result)
