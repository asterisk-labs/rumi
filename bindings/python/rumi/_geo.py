from typing import NamedTuple

from ._ffi import PathLike, _check, _enc, ffi, lib


class Geo(NamedTuple):
    """Georeferencing stored in a rumi file.

    ``transform`` follows the order ``(x_res, row_rot, x_origin, col_rot,
    y_res, y_origin)``. ``crs`` is an EPSG code. Both are ``None`` when the
    file has no georeferencing. ``pixel_is_point`` is true when coordinates
    refer to pixel centers rather than their upper-left corners.
    """

    transform: tuple[float, ...] | None
    crs: int | None
    pixel_is_point: bool


def read_geo(path: PathLike) -> Geo:
    """Read georeferencing from a local rumi file."""
    coeffs = ffi.new("double[6]")
    out_crs = ffi.new("uint32_t*")
    out_point = ffi.new("int*")
    _check(lib.rumi_read_geo(_enc(path), coeffs, out_crs, out_point))

    crs = int(out_crs[0]) or None
    transform = None if crs is None else tuple(float(value) for value in coeffs)
    return Geo(transform, crs, bool(out_point[0]))
