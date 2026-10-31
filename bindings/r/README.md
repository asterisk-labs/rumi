# rumi for R

R reader for Rumi Image and Cube files. The external header is always paired
with the path when a dataset is opened.

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
