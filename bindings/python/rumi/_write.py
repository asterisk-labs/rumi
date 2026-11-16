import operator
from collections.abc import Iterable, Set
from itertools import islice

import numpy as np

from ._dtype import dtype_code
from ._ffi import _check, c_int, ffi, lib
from ._native import FilePath, encode_path
from ._pattern import compile_pattern, frame_count
from ._time import compile_axis


def _integer(value, message: str) -> int:
    # bool is an int, so it would otherwise pass as 0 or 1.
    if isinstance(value, (bool, np.bool_)):
        raise TypeError(message)
    try:
        return operator.index(value)
    except TypeError:
        raise TypeError(message) from None


def _epsg(crs) -> int:
    """Return an EPSG code accepted by the writer."""
    if isinstance(crs, (bool, np.bool_)):
        raise TypeError("crs must be an EPSG code")
    code = None
    if isinstance(crs, (int, np.integer)):
        code = int(crs)
    elif isinstance(crs, str):
        text = crs.strip()
        if text.upper().startswith("EPSG:"):
            text = text[5:]
        if text.isascii() and text.isdigit():
            code = int(text)
    elif callable(getattr(crs, "to_epsg", None)):
        code = crs.to_epsg()
    if code is None:
        raise ValueError(f"crs must be an EPSG code, got {crs!r}")
    # The core reads EPSG 0 as a file without a CRS, so it cannot name one.
    if code == 0:
        raise ValueError("crs must be an EPSG code from 1 to 4294967295, got 0")
    return c_int(int(code), "crs", "uint32_t")


def _desc(tf, transform, crs, pixel_is_point):
    """Build a write descriptor and retain its referenced buffers."""
    keep = []
    d = ffi.new("rumi_write_desc*")
    d.image_width = tf.image_width
    d.image_length = tf.image_length
    d.time_count = tf.time_count
    d.tile_size = tf.tile_size
    d.samples_per_pixel = tf.bands
    d.dtype = dtype_code(tf.dtype)
    if not isinstance(pixel_is_point, (bool, np.bool_)):
        raise TypeError(
            f"pixel_is_point must be a bool, got {type(pixel_is_point).__name__}")
    d.pixel_is_point = 1 if pixel_is_point else 0
    d.frame_unit = tf.frame_unit

    d.transform = ffi.NULL
    d.epsg = 0 if crs is None else _epsg(crs)
    if transform is not None:
        values = tuple(transform)
        if len(values) not in (6, 9):
            raise ValueError(
                f"a transform is six coefficients (x_res, row_rot, x_origin, "
                f"col_rot, y_res, y_origin), got {len(values)}")
        if len(values) == 9 and values[6:] != (0.0, 0.0, 1.0):
            raise ValueError("a nine-value affine transform must end in (0, 0, 1)")
        coeffs = ffi.new("double[6]", [float(v) for v in values[:6]])
        keep.append(coeffs)
        d.transform = coeffs
    return d, keep


def _band_text(text, b) -> bytes:
    """Encode one band text as the C string the core receives."""
    if not isinstance(text, str):
        raise TypeError(
            f"a band text is a str, got {type(text).__name__} for band {b}")
    if "\x00" in text:
        raise ValueError(
            f"band {b} text contains a NUL character, which a C string "
            f"cannot carry")
    try:
        return text.encode("utf-8")
    except UnicodeEncodeError:
        raise ValueError(
            f"band {b} text cannot be encoded as UTF-8") from None


def _label(d, keep, tf, bands, time):
    """Point the descriptor at the band texts and time coordinates."""
    if bands is None:
        raise TypeError(
            "every file names its bands; pass one text per band, in band order")
    if isinstance(bands, Set):
        raise TypeError("bands must be ordered; a set cannot preserve band order")
    if isinstance(bands, (str, bytes)) or not hasattr(bands, "__iter__"):
        raise TypeError(
            f"bands is a list with one text per band; wrap a single text as "
            f"bands=[{bands!r}]")
    # Limit generators to one more band name than expected.
    names = list(islice(bands, tf.bands + 1))
    if len(names) > tf.bands:
        noun = "band" if tf.bands == 1 else "bands"
        raise ValueError(
            f"a file with {tf.bands} {noun} needs one text per band; "
            f"got more than {tf.bands}")
    texts = [ffi.new("char[]", _band_text(text, b))
             for b, text in enumerate(names)]
    keep.extend(texts)
    array = ffi.new("const char*[]", texts)
    keep.append(array)
    d.band_texts = array
    d.band_text_count = len(texts)

    # Pass POSIX seconds to the core; it selects the canonical trailer scale.
    kind, coords = compile_axis(time, tf.time_count)
    seconds = ffi.new("int64_t[]", [int(c) for c in coords])
    keep.append(seconds)
    d.time_type = kind
    d.time = seconds
    d.time_coords = len(coords)


def header_bytes(tf, *, transform=None, crs=None,
                 pixel_is_point=False) -> int:
    """Return the first frame offset for this layout."""
    d, _keep = _desc(tf, transform, crs, pixel_is_point)
    out = ffi.new("uint64_t*")
    _check(lib.rumi_write_base_offset(d, out))
    return int(out[0])


def frame_start(shape, pattern, tile_size=512) -> int:
    """Return the byte offset of the first frame without materializing frames.

    ``shape`` gives the raster dimensions in the axis order named by
    ``pattern``. ``tile_size`` is the nominal height and width of a square
    tile, from 1 to 65535.

    The result is also the size of everything the file stores before its
    first frame.
    """
    p = compile_pattern(pattern)
    dims = tuple(_integer(value, "shape must contain integers") for value in shape)
    if len(dims) != len(p.input_axes):
        raise ValueError(
            f"the pattern names {len(p.input_axes)} axes "
            f"({' '.join(p.input_axes)}), got shape {dims}")
    tile = c_int(_integer(tile_size, "tile_size must be an integer"), "tile_size",
                 "uint16_t")

    extents = dict(zip(p.input_axes, dims, strict=True))
    bands = c_int(extents.get("b", 1), "band count", "uint16_t")
    times = c_int(extents.get("t", 1), "time", "uint32_t")
    length = c_int(extents["y"], "height", "uint32_t")
    width = c_int(extents["x"], "width", "uint32_t")

    unit = p.frame_unit(bands, times)
    count = frame_count(unit, width, length, tile, bands, times)[2]
    out = ffi.new("uint64_t*")
    _check(lib.rumi_frame_start(bands, count, out))
    return int(out[0])


def write_frames(path: FilePath, frames: Iterable[bytes], tf, *, bands, time,
                 transform=None, crs=None, pixel_is_point=False) -> bytes:
    """Write compressed frames and return the binary header."""
    frames = [ffi.from_buffer(f) for f in frames]
    d, keep = _desc(tf, transform, crs, pixel_is_point)
    _label(d, keep, tf, bands, time)
    ptrs = ffi.new("unsigned char*[]", [ffi.cast("unsigned char*", f)
                                        for f in frames])
    sizes = ffi.new("size_t[]", [len(f) for f in frames])

    out = ffi.new("unsigned char**")
    size = ffi.new("size_t*")
    _check(lib.rumi_write(encode_path(path), d, ptrs, sizes, len(frames), out, size))
    del keep
    try:
        return bytes(ffi.buffer(out[0], size[0]))
    finally:
        lib.rumi_free(out[0])


def write(path, frames, *, bands, time, transform=None, crs=None,
          pixel_is_point=False) -> bytes:
    """Write a compressed FrameTable to a rumi file.

    ``path`` is a local filesystem path. It must be text or a path-like object
    returning text. A leading ``~`` is expanded.

    ``frames`` is a ``FrameTable`` with a non-empty compressed payload
    assigned to every frame. Rumi stores those payloads without decoding or
    recompressing them.

    ``bands`` contains one non-empty, unique text per band, in band order. A
    useful label includes the name, description and wavelength, such as
    ``"B4, Red, 665nm"``.

    ``time`` contains one entry per time step. An instant may be a date,
    datetime, ISO string or ``numpy.datetime64``. ISO text is ``YYYY-MM-DD``,
    or a date and ``HH:MM`` joined by ``T`` or a space, with optional seconds
    and an optional ``Z``, ``+HH``, ``+HHMM`` or ``+HH:MM`` offset. A fraction
    of a second must be all zeros. An interval is a ``(start, end)`` pair
    using the same value types.

    ``transform`` contains six affine coefficients in rasterio order:
    ``(x_res, row_rot, x_origin, col_rot, y_res, y_origin)``. It must be given
    together with ``crs``, an EPSG integer from 1 to 4294967295, an
    ``"EPSG:32718"`` string or an object implementing ``to_epsg()``.

    ``pixel_is_point`` is a bool that records whether coordinates refer to
    pixel centres. The default records PixelIsArea, whose coordinates refer
    to pixel corners.

    Returns the external header as bytes, the value ``read`` and
    ``read_many`` need beside the file. A failure after the file is opened
    removes the incomplete output.
    """
    payloads = frames["compressed"]
    missing = [i for i, f in enumerate(payloads) if f is None]
    if missing:
        raise ValueError(
            f"{len(missing)} of {len(payloads)} frames have no payload, first is "
            f"{missing[0]}; compress every frame before writing")

    return write_frames(path, payloads, frames, bands=bands, time=time,
                        transform=transform, crs=crs,
                        pixel_is_point=pixel_is_point)
