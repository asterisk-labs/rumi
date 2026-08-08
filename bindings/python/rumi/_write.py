from ._assemble import assemble
from ._ffi import _blob_from_file


def write(path, tf, *, transform=None, crs=None, pixel_is_point=False,
          header_size="auto"):
    """Write a compressed TileFrame to a rumi file.

    Returns (path, header_blob), the blob being the bytes you cache in a
    catalog or Parquet and hand back to read.

    tf              a TileFrame with every tile compressed. rumi does not
                    compress, that is the caller's loop and the caller's choice
                    of graph.
    transform       GDAL-style affine geotransform (6 coeffs); pairs with crs.
    crs             EPSG int or projection string; pairs with transform.
    pixel_is_point  anchor the pixel at its center (PixelIsPoint) rather than
                    its top-left corner (PixelIsArea, the default).
    header_size     round the tile-data offset up to this multiple for
                    alignment, or "auto" to pack tight.
    """
    frames = tf["compressed"]
    missing = [i for i, f in enumerate(frames) if f is None]
    if missing:
        raise ValueError(
            f"{len(missing)} of {len(frames)} tiles have no frame, first is "
            f"{missing[0]}; compress every tile before writing")

    assemble(path, frames, tf, transform=transform, crs=crs,
             pixel_is_point=pixel_is_point, header_size=header_size)
    return path, _blob_from_file(path)
