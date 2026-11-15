import operator
from typing import Any

import numpy as np
from numpy.typing import ArrayLike

from ._dtype import dtype_code
from ._ffi import _check, c_int, ffi, lib

Error = int | float | str | None


def _is_native(dt: np.dtype) -> bool:
    return dt.byteorder in ("=", "|") or dt.newbyteorder("=") == dt


def _samples(tile: ArrayLike) -> tuple[np.ndarray, int]:
    arr = np.ascontiguousarray(tile)
    if not _is_native(arr.dtype):
        raise ValueError("samples must be in native byte order; byte-swap them first")
    return arr, dtype_code(arr.dtype)


def _error_text(error: Error) -> bytes | None:
    if error is None:
        return None
    if isinstance(error, (bool, np.bool_)):
        raise TypeError("error must be a number, a percentage or a recipe, not a bool")
    if isinstance(error, (int, np.integer)):
        return str(int(error)).encode()
    if isinstance(error, (float, np.floating)):
        return str(error).encode()
    if isinstance(error, str):
        return error.encode("utf-8")
    raise TypeError("error must be a number, a percentage or a recipe")


def _nodata_sample(value: Any, dtype: np.dtype) -> bytes:
    v = np.asarray(value)
    if v.ndim != 0:
        raise TypeError("nodata must be one sample")
    # Preserve the bit pattern of a NumPy scalar, including signaling NaNs.
    if isinstance(value, (np.generic, np.ndarray)) and value.dtype != dtype:
        value = value.item()
    if dtype.kind in "iu" and v.dtype.kind == "f" and v != np.floor(v):
        raise ValueError("nodata must be a whole number on an integer raster")
    if (dtype.kind in "iu" and v.dtype.kind in "iufO" and
            not np.iinfo(dtype).min <= v.item() <= np.iinfo(dtype).max):
        raise OverflowError("nodata does not fit the dtype")
    with np.errstate(over="raise"):
        return np.asarray(value, dtype=dtype).tobytes()


class Graph:
    """A reusable compression graph returned by :func:`graph`.

    Its native context is not safe for concurrent calls.
    """

    __slots__ = ("_h", "_dtype", "method", "width", "planes", "error", "nodata")

    _h: Any
    _dtype: np.dtype
    method: str
    width: int
    planes: int
    error: Error
    nodata: Any

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        raise TypeError("rumi.geozl.Graph is returned by rumi.geozl.graph and "
                        "cannot be created directly")

    @classmethod
    def _new(cls, handle: Any, *, method: str, width: int, planes: int,
             dtype: np.dtype, error: Error, nodata: Any) -> "Graph":
        g = object.__new__(cls)
        g._h = handle
        g.method = method
        g.width = width
        g.planes = planes
        g._dtype = dtype
        g.error = error
        g.nodata = nodata
        return g

    @property
    def dtype(self) -> np.dtype:
        return self._dtype


def graph(raster: ArrayLike, method: str, *, width: int | None = None,
          planes: int | None = None, error: Error = None,
          nodata: Any = None) -> Graph:
    """Build a graph from a raster and a GeoZL recipe.

    Row width defaults to the last axis; planes default to the first axis for
    arrays with at least three dimensions. ``error`` accepts a number (LINEAR),
    a percentage (LOG), or a full recipe. None and zero mean lossless.
    The raster sets the domain for later lossy tiles. ``nodata`` marks missing
    samples; without it, NaNs in a float raster are detected automatically.
    """
    if not isinstance(method, str):
        raise TypeError("method must be a GeoZL graph such as 'planar>zigzag>pfor'")
    text = _error_text(error)
    arr, code = _samples(raster)
    if width is None:
        if arr.ndim < 2:
            raise ValueError("give width for a 1d raster")
        width = arr.shape[-1]
    if planes is None:
        planes = arr.shape[0] if arr.ndim >= 3 else 1
    try:
        width, planes = operator.index(width), operator.index(planes)
    except TypeError:
        raise TypeError("width and planes must be integers") from None
    sample = None if nodata is None else _nodata_sample(nodata, arr.dtype)

    out = ffi.new("rumi_geozl_graph**")
    _check(lib.rumi_geozl_graph_open(
        method.encode("utf-8"), code, ffi.from_buffer(arr), arr.nbytes,
        c_int(width, "width", "uint32_t"), c_int(planes, "planes", "uint32_t"),
        ffi.NULL if text is None else text,
        ffi.NULL if sample is None else ffi.from_buffer(sample), out))
    return Graph._new(ffi.gc(out[0], lib.rumi_geozl_graph_close),
                      method=method, width=width, planes=planes, dtype=arr.dtype,
                      error=error, nodata=nodata)


def compress(tile: ArrayLike, *, graph: Graph) -> bytes:
    """Compress a tile with a prepared graph and return its frame bytes.

    The dtype must match; geometry is not checked. A lossy tile outside the
    graph's domain is rejected.
    """
    if not isinstance(graph, Graph):
        raise TypeError("graph must come from rumi.geozl.graph")
    arr, code = _samples(tile)
    out = ffi.new("unsigned char**")
    size = ffi.new("size_t*")
    _check(lib.rumi_geozl_compress(graph._h, code, ffi.from_buffer(arr), arr.nbytes,
                                   out, size))
    try:
        return bytes(ffi.buffer(out[0], size[0]))
    finally:
        lib.rumi_free(out[0])
