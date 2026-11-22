# rumi for R

R reader and writer for Rumi Image and Cube files. Every read takes the source,
a path or the file's bytes as a raw vector, together with its external header.

The source package builds its bundled native code. It needs CMake, libcurl and
OpenSSL; no separate Rumi installation is required.

```r
library(rumi)

header <- readBin("scene.header", "raw", file.info("scene.header")$size)
chip <- rumi_read("scene.rumi", header, time = 1:3, bands = c(4, 8),
                  window = c(1, 1, 256, 256))
# Checks the pair and returns the band texts and time coordinates.
metadata <- rumi_info("scene.rumi", header)

# A file without its external header rebuilds it.
header <- rumi_info("scene.rumi")$header
```

Selections and window origins are one-based. The window order is
`(row, column, height, width)`. An Image is returned as an array shaped
`(X, Y, Band)` and a Cube as `(X, Y, Band, Time)`. A read uses the header
alone, so the array has no band or time dimnames, and its `rumi_metadata`
attribute holds the header's fields. The affine transform, CRS, band texts and
time coordinates come from `rumi_info(source, header)`. Metadata keeps Rumi's
canonical `(Band, Y, X)` or `(Time, Band, Y, X)` shape.

Native failures other than a bad request signal a condition of class
`rumi_error_<kind>` and `rumi_error`, such as `rumi_error_io`, so
`tryCatch(..., rumi_error_io = ...)` can retry a network read.

## Writing

`rumi_frames()` cuts an array into frames, you compress each one with the
[GeoZL](https://asterisk.coop/geozl/) built into the package, and `rumi_write()`
stores the payloads and returns the external header.

```r
library(rumi)

image <- array(sample(0:4095, 1024 * 1024 * 4, TRUE), c(1024, 1024, 4))
frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                      tile_size = 256, dtype = "uint16")
for (i in seq_along(frames$data)) {
  tile <- frames$data[[i]]
  graph <- rumi_geozl_graph(tile, "planar>zigzag>pfor")
  frames$compressed[[i]] <- rumi_geozl_compress(tile, graph)
}
header <- rumi_write("scene.rumi", frames, bands = c("B02", "B03", "B04", "B08"),
                     time = "2025-08-08",
                     transform = c(10, 0, 500000, 0, -10, 8000000), crs = 32618)
writeBin(header, "scene.header")
```

Patterns use the same axis names as Python. R holds those axes in reverse, so
this image is `(X, Y, Band)`, and each frame is ready for `rumi_geozl_graph()`.

`rumi_geozl_graph()` and `rumi_geozl_compress()` refuse bad arguments in the
words of the [geozl](https://asterisk.coop/geozl/) packages. They cover graph
and compress only. Decompress, profile and coeffs live in the geozl package.
