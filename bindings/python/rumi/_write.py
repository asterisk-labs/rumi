from collections.abc import Iterable

from ._dtype import dtype_code
from ._ffi import PathLike, _check, _enc, ffi, lib


def _epsg(crs) -> int:
    """rumi writes a CRS as an EPSG code, which is the whole reference, and
    leaves everything else to the reader's own EPSG tables."""
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
    raise ValueError(
        f"crs must be an EPSG code, got {crs!r}. rumi does not parse WKT or "
        "PROJ strings; pass the code, or the GeoTIFF keys for a CRS no code "
        "names.")


def _desc(tf, transform, crs, pixel_is_point):
    """Fill a rumi_write_desc. Returns it with the buffers it points at, which
    the caller has to hold until the call returns."""
    if (transform is None) != (crs is None):
        raise ValueError("transform and crs must be given together")

    keep = []
    d = ffi.new("rumi_write_desc*")
    d.image_width = tf.image_width
    d.image_length = tf.image_length
    d.tile_size = tf.tile_size
    d.samples_per_pixel = tf.bands
    d.dtype = dtype_code(tf.dtype)
    d.pixel_is_point = 1 if pixel_is_point else 0
    d.frame_unit = 0 if tf.unit == "tile" else 1

    if transform is None:
        d.transform = ffi.NULL
        d.epsg = 0
    else:
        coeffs = ffi.new("double[6]", [float(v) for v in tuple(transform)[:6]])
        keep.append(coeffs)
        d.transform = coeffs
        d.epsg = _epsg(crs)
    return d, keep


def header_bytes(tf, *, transform=None, crs=None,
                 pixel_is_point=False) -> int:
    """Where the frame data will start, without writing. The tag set is fixed,
    so this is arithmetic over the shape and never reads a file."""
    d, _keep = _desc(tf, transform, crs, pixel_is_point)
    out = ffi.new("uint64_t*")
    _check(lib.rumi_write_base_offset(d, out))
    return int(out[0])


def write_frames(path: PathLike, frames: Iterable[bytes], tf, *,
                 transform=None, crs=None, pixel_is_point=False) -> bytes:
    """Write the file and return its header. tf is a FrameTable, and the five
    fields it carries are the header; the frames come in its own order, which
    is the wire order."""
    frames = [ffi.from_buffer(f) for f in frames]
    d, keep = _desc(tf, transform, crs, pixel_is_point)
    ptrs = ffi.new("unsigned char*[]", [ffi.cast("unsigned char*", f)
                                        for f in frames])
    sizes = ffi.new("size_t[]", [len(f) for f in frames])

    out = ffi.new("unsigned char**")
    size = ffi.new("size_t*")
    _check(lib.rumi_write(_enc(path), d, ptrs, sizes, len(frames), out, size))
    del keep
    try:
        return bytes(ffi.buffer(out[0], size[0]))
    finally:
        lib.rumi_free(out[0])


def write(path, tf, *, transform=None, crs=None, pixel_is_point=False):
    """Write a compressed FrameTable to a rumi file.

    Returns (path, header), the header being the bytes you cache in a catalog
    or Parquet and hand back to read.

    tf              a FrameTable with every frame compressed. rumi does not
                    compress, that is the caller's loop and the caller's choice
                    of graph.
    transform       affine coefficients (x_res, row_rot, x_origin, col_rot,
                    y_res, y_origin); pairs with crs.
    crs             EPSG code, as an int, "EPSG:32718", or any object with
                    a to_epsg(); pairs with transform.
    pixel_is_point  anchor the pixel at its center (PixelIsPoint) rather than
                    its top-left corner (PixelIsArea, the default).
    """
    frames = tf["compressed"]
    missing = [i for i, f in enumerate(frames) if f is None]
    if missing:
        raise ValueError(
            f"{len(missing)} of {len(frames)} frames have no payload, first is "
            f"{missing[0]}; compress every frame before writing")

    header = write_frames(path, frames, tf, transform=transform, crs=crs,
                          pixel_is_point=pixel_is_point)
    return path, header
