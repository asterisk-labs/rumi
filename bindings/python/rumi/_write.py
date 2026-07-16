from ._assemble import assemble, tile
from ._ffi import _blob_from_file, lib

_OPENZL_VERSION = lib.rumi_openzl_format_version()


def write(path, arr, *, method="full", chunk_size=512, max_error=None,
          transform=None, crs=None, pixel_is_point=False, header_size="auto"):
    """Tile arr, compress each tile keeping the smallest frame, and assemble the
    rumi file. Returns (path, header_blob), the blob being the bytes you cache in
    a catalog or Parquet and hand back to read.

    method        "full" sweeps every predictor per tile and keeps the smallest,
                  or a single predictor: delta_w, delta_n, planar, med, average,
                  wp_static, delta_1d. Validated by geozl.
    chunk_size    tile edge in pixels, a multiple of 16.
    max_error     if set, lossy quant_linear ahead of the predictor, an absolute
                  error bound in DN; None keeps the write lossless.
    transform     GDAL-style affine geotransform (6 coeffs); pairs with crs.
    crs           EPSG int or projection string; pairs with transform.
    pixel_is_point  anchor the pixel at its center (PixelIsPoint) rather than
                  its top-left corner (PixelIsArea, the default).
    header_size   round the tile-data offset up to this multiple for alignment,
                  or "auto" to pack tight. Not the same as chunk_size.
    """
    import geozl

    chunks, layout = tile(arr, chunk_size)

    # geozl.compress defaults to its own newest format version; pin it to the one
    # librumi was built against, or read rejects the frame at that version.
    frames = [
        geozl.compress(ch, method=method, max_error=max_error,
                       format_version=_OPENZL_VERSION)
        for ch in chunks
    ]
    assemble(path, frames, layout, transform=transform, crs=crs,
             pixel_is_point=pixel_is_point, header_size=header_size)
    return path, _blob_from_file(path)