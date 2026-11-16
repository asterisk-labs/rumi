import math
import numbers
import operator
from decimal import Decimal
from typing import Any

import numpy as np
from numpy.typing import ArrayLike

from ._dtype import dtype_code
from ._ffi import _check, c_int, ffi, lib

__all__ = ["Graph", "compress", "graph"]

Error = int | float | str | None

# width and planes cross the C API as uint32_t
_UINT32_MAX = 2**32 - 1

# The argument checks below raise the geozl package's classes and texts, so both
# packages refuse the same input the same way. The core checks the rest.


def _is_native(dt: np.dtype) -> bool:
    return dt.byteorder in ("=", "|") or dt.newbyteorder("=") == dt


def _samples(tile: ArrayLike) -> tuple[np.ndarray, int]:
    arr = np.ascontiguousarray(tile)
    if arr.dtype.hasobject:
        raise ValueError("object arrays hold references, not samples")
    if not _is_native(arr.dtype):
        raise ValueError(f"dtype {arr.dtype} is not native byte order; the "
                         f"kernels read native, byte-swap it first")
    return arr, dtype_code(arr.dtype)


def _error_text(error: Error) -> bytes | None:
    if error is None:
        return None
    if isinstance(error, (bool, np.bool_)):
        raise ValueError("error must not be a boolean")
    if isinstance(error, (int, np.integer)):
        return str(int(error)).encode()
    if isinstance(error, (float, np.floating)):
        return str(error).encode()
    if not isinstance(error, str):
        raise ValueError(f"error must be a number, percentage or recipe, got "
                         f"{error!r}")
    if "\x00" in error:
        raise ValueError("error must not contain a NUL character")
    # A bare number is LINEAR only as a number, as in the geozl package.
    if ":" not in error and not error.endswith("%"):
        raise ValueError(f"error must be a number, a percentage such as \"10%\", "
                         f"or a LINEAR, LOG or SQRT recipe, got {error!r}")
    return error.encode("utf-8")


def _count(value: Any, name: str) -> int:
    # index() refuses floats and strings, where int() would truncate 3.7.
    if isinstance(value, (bool, np.bool_)):
        raise TypeError(f"{name} must be a whole number")
    try:
        number = operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be a whole number") from None
    if not 1 <= number <= _UINT32_MAX:
        raise ValueError(f"{name} must be from 1 to {_UINT32_MAX}")
    return number


def _nodata_sample(value: Any, dtype: np.dtype) -> bytes:
    if isinstance(value, np.ndarray):
        number = value.ndim == 0 and value.dtype.kind in "iuf"
    else:
        number = (isinstance(value, (numbers.Real, Decimal)) and
                  not isinstance(value, (bool, np.bool_)))
    if not number:
        raise TypeError("nodata must be a number")
    # A signaling Decimal NaN raises on every comparison, so it becomes the
    # float a quiet one converts to.
    if isinstance(value, Decimal) and value.is_nan():
        value = float("-nan" if value.is_signed() else "nan")
    # The core refuses a sentinel on these whatever its value, in geozl's words.
    if dtype == np.float16 or dtype.kind not in "iuf":
        return bytes(dtype.itemsize)
    # Preserve the bit pattern of a NumPy scalar, including signaling NaNs.
    if isinstance(value, (np.generic, np.ndarray)) and value.dtype != dtype:
        value = value.item()
    if dtype.kind in "iu":
        # Python arithmetic checks a Fraction or a Decimal exactly, never truncated.
        x = value.item() if isinstance(value, (np.generic, np.ndarray)) else value
        if x != x:
            raise ValueError(f"a {dtype} raster cannot use NaN as nodata")
        if abs(x) != math.inf and x != int(x):
            raise ValueError(f"nodata {value} is not a whole number, it cannot "
                             f"be a sentinel on {dtype}")
        if not np.iinfo(dtype).min <= x <= np.iinfo(dtype).max:
            raise ValueError("nodata does not fit the dtype")
        return np.asarray(int(x), dtype=dtype).tobytes()
    try:
        with np.errstate(over="raise"):
            packed = np.asarray(value, dtype=dtype)
    except (OverflowError, FloatingPointError):
        raise ValueError("nodata does not fit the dtype") from None
    # NumPy turns a Decimal past the dtype into infinity without a warning.
    if np.isinf(packed) and abs(np.asarray(value).item()) != math.inf:
        raise ValueError("nodata does not fit the dtype")
    return packed.tobytes()


class Graph:
    """A reusable compression graph returned by :func:`graph`.

    It keeps ``method``, ``width``, ``planes``, ``dtype``, ``itemsize`` (bytes
    per sample), ``error`` and ``nodata``. Its native context is not safe for
    concurrent calls.
    """

    __slots__ = ("_h", "_dtype", "method", "width", "planes", "itemsize", "error",
                 "nodata")

    _h: Any
    _dtype: np.dtype
    method: str
    width: int
    planes: int
    itemsize: int
    error: str | None
    nodata: Any

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        raise TypeError("rumi.geozl.Graph is returned by rumi.geozl.graph and "
                        "cannot be created directly")

    @classmethod
    def _new(cls, handle: Any, *, method: str, width: int, planes: int,
             dtype: np.dtype, error: str | None, nodata: Any) -> "Graph":
        g = object.__new__(cls)
        g._h = handle
        g.method = method
        g.width = width
        g.planes = planes
        g._dtype = dtype
        g.itemsize = dtype.itemsize
        g.error = error
        g.nodata = nodata
        return g

    @property
    def dtype(self) -> np.dtype:
        return self._dtype

    def __repr__(self) -> str:
        plane_word = "plane" if self.planes == 1 else "planes"
        return (f"<rumi.geozl.Graph {self.method}, {self.dtype}, row width "
                f"{self.width}, {self.planes} {plane_word}, "
                f"{self.error or 'lossless'}>")


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
    if not isinstance(method, str) or not method:
        raise ValueError(f"method must be a recipe name, got {method!r}")
    if "\x00" in method:
        raise ValueError("method must not contain a NUL character")
    text = _error_text(error)
    arr, code = _samples(raster)
    if width is None:
        if arr.ndim < 2:
            raise ValueError("give width for a 1d raster")
        width = arr.shape[-1]
    else:
        width = _count(width, "width")
    if planes is None:
        planes = arr.shape[0] if arr.ndim >= 3 else 1
    planes = _count(planes, "planes")
    sample = None if nodata is None else _nodata_sample(nodata, arr.dtype)

    out = ffi.new("rumi_geozl_graph**")
    _check(lib.rumi_geozl_graph_open(
        method.encode("utf-8"), code, ffi.from_buffer(arr), arr.nbytes,
        c_int(width, "width", "uint32_t"), c_int(planes, "planes", "uint32_t"),
        ffi.NULL if text is None else text,
        ffi.NULL if sample is None else ffi.from_buffer(sample), out))
    handle = ffi.gc(out[0], lib.rumi_geozl_graph_close)
    # The core normalizes the short syntax, so the graph reports it as GeoZL does.
    recipe = lib.rumi_geozl_graph_error(handle)
    return Graph._new(handle, method=method, width=width, planes=planes,
                      dtype=arr.dtype, nodata=nodata,
                      error=None if recipe == ffi.NULL else ffi.string(recipe).decode())


def compress(tile: ArrayLike, graph: Graph) -> bytes:
    """Compress a tile with a prepared graph and return its frame bytes.

    ``graph`` may be given by position or by name. The dtype must match;
    geometry is not checked. A lossy tile outside the graph's domain is
    rejected.
    """
    if not isinstance(graph, Graph):
        raise TypeError(f"graph must be a rumi.geozl.Graph, got "
                        f"{type(graph).__name__}")
    arr, code = _samples(tile)
    out = ffi.new("unsigned char**")
    size = ffi.new("size_t*")
    _check(lib.rumi_geozl_compress(graph._h, code, ffi.from_buffer(arr), arr.nbytes,
                                   out, size))
    try:
        return bytes(ffi.buffer(out[0], size[0]))
    finally:
        lib.rumi_free(out[0])
