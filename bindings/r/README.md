# rumi for R

R reader and writer for Rumi Image and Cube files. The external header is
always paired with the path when a dataset is opened.

The source package builds its bundled native code. It needs CMake, libcurl and
OpenSSL; no separate Rumi installation is required.

```r
library(rumi)

header <- readBin("scene.header", "raw", file.info("scene.header")$size)
file <- rumi_open("scene.rumi", header)
chip <- rumi_read(file, bands = c(4, 8), time = 1:3,
                  window = c(1, 1, 256, 256))
metadata <- rumi_info(file)
rumi_close(file)
```

Selections and window origins are one-based. The window order is
`(row, column, height, width)`. An Image is returned as an array shaped
`(X, Y, Band)` and a Cube as `(X, Y, Band, Time)`. The exact affine transform,
CRS, band descriptions and time coordinates stay in `rumi_info(file)` and in
the array's `rumi_metadata` attribute. Metadata keeps Rumi's canonical
`(Band, Y, X)` or `(Time, Band, Y, X)` shape.

## Writing

`rumi_frames()` cuts an array into frames, you compress each one with the
[GeoZL](https://asterisk.coop/geozl/) built into the package, and `rumi_write()`
stores the payloads and returns the external header.

```r
library(rumi)

image <- array(sample(0:4095, 1024 * 1024 * 4, TRUE), c(1024, 1024, 4))
frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                      tile_size = 256, datatype = "uint16")
for (i in seq_along(frames$data)) {
  tile <- frames$data[[i]]
  graph <- rumi_geozl_graph(tile, "planar>zigzag>pfor", datatype = "uint16")
  frames$compressed[[i]] <- rumi_geozl_compress(tile, graph)
}
header <- rumi_write("scene.rumi", frames, bands = c("B02", "B03", "B04", "B08"),
                     time = "2025-08-08",
                     transform = c(10, 0, 500000, 0, -10, 8000000), crs = 32618)
writeBin(header, "scene.header")
```

Patterns use the same axis names as Python. R holds those axes in reverse:
this image is `(X, Y, Band)`, and each frame is ready for `rumi_geozl_graph()`.
