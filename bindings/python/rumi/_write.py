from collections.abc import Iterable, Set
from itertools import islice

from ._dtype import dtype_code
from ._ffi import _check, ffi, lib
from ._native import FilePath, encode_path
from ._time import compile_axis


def _epsg(crs) -> int:
    """Return an EPSG code accepted by the writer."""
    if isinstance(crs, bool):
        raise TypeError("crs must be an EPSG code")
    if isinstance(crs, int):
        return crs
    if isinstance(crs, str):
        text = crs.strip()
        if text.upper().startswith("EPSG:"):
            text = text[5:]
        if text.isdigit():
            return int(text)
    code = getattr(crs, "to_epsg", None)
    if callable(code):
        got = code()
        if got:
            return int(got)
    raise ValueError(f"crs must be an EPSG code, got {crs!r}")


def _desc(tf, transform, crs, pixel_is_point):
    """Build a write descriptor and retain its referenced buffers."""
    if (transform is None) != (crs is None):
        raise ValueError("transform and crs must be given together")

    keep = []
    d = ffi.new("rumi_write_desc*")
    d.image_width = tf.image_width
    d.image_length = tf.image_length
    d.time_count = tf.time_count
    d.tile_size = tf.tile_size
    d.samples_per_pixel = tf.bands
    d.dtype = dtype_code(tf.dtype)
    d.pixel_is_point = 1 if pixel_is_point else 0
    d.frame_unit = tf.frame_unit

    if transform is None:
        d.transform = ffi.NULL
        d.epsg = 0
    else:
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
        d.epsg = _epsg(crs)
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


def write(path, tf, *, bands, time, transform=None, crs=None,
          pixel_is_point=False):
    """Write a compressed FrameTable to a rumi file.

    ``path`` is a local filesystem path. It must be text or a path-like object
    returning text.

    ``tf`` is a ``FrameTable`` with a non-empty compressed payload assigned to
    every frame. Rumi stores those payloads without decoding or recompressing
    them.

    ``bands`` contains one non-empty, unique text per band, in band order. A
    useful label includes the name, description and wavelength, such as
    ``"B4, Red, 665nm"``.

    ``time`` contains one entry per time step. An instant may be a date,
    datetime, ISO string or ``numpy.datetime64``. An interval is a
    ``(start, end)`` pair using the same value types.

    ``transform`` contains six affine coefficients in rasterio order:
    ``(x_res, row_rot, x_origin, col_rot, y_res, y_origin)``. It must be given
    together with ``crs``, an EPSG integer, an ``"EPSG:32718"`` string or an
    object implementing ``to_epsg()``.

    ``pixel_is_point`` records whether coordinates refer to pixel centres.
    The default records PixelIsArea, whose coordinates refer to pixel corners.

    Returns ``(path, header)`` with ``path`` unchanged and ``header`` as the
    external bytes needed by ``read`` and ``read_many``. A failure after the
    file is opened removes the incomplete output.
    """
    frames = tf["compressed"]
    missing = [i for i, f in enumerate(frames) if f is None]
    if missing:
        raise ValueError(
            f"{len(missing)} of {len(frames)} frames have no payload, first is "
            f"{missing[0]}; compress every frame before writing")

    header = write_frames(path, frames, tf, bands=bands, time=time,
                          transform=transform, crs=crs,
                          pixel_is_point=pixel_is_point)
    return path, header
