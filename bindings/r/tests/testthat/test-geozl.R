library(rumi)

# The rasters tools/make_binding_fixture.py compresses with the GeoZL Python
# package, rebuilt by the same formulas in R's (x, y, band) order.
grid <- function(nx, ny, nb = 1L) {
  expand.grid(x = seq_len(nx) - 1, y = seq_len(ny) - 1, b = seq_len(nb) - 1)
}

counts <- local({
  g <- grid(40, 48, 3)
  array(200 + g$b * 977 + g$y * 31 + g$x * 17 +
          (g$x * 7919 + g$y * 104729 + g$b * 31) %% 97, c(40, 48, 3))
})
reflectance <- local({
  g <- grid(40, 48)
  matrix(((g$y * 131 + g$x * 71) %% 1000) / 4000 + 0.1, 40, 48)
})
holes <- local({
  g <- grid(40, 48)
  x <- reflectance
  x[(g$y * 40 + g$x) %% 97 == 0] <- NaN
  x
})
radiance <- local({
  g <- grid(128, 128)
  matrix(50 + 4000 * (g$x + g$y) / 254 + (g$x * 7919 + g$y * 104729) %% 61,
         128, 128)
})

cases <- list(
  lossless = list(counts, "planar>zigzag>zstd", "uint16", list()),
  linear = list(counts, "planar>zigzag>pfor", "uint16", list(error = 3)),
  `linear-float` = list(reflectance, "planar>zigzag>zstd", "float32",
                        list(error = 0.01)),
  log = list(reflectance, "planar>zigzag>zstd", "float32", list(error = "1%")),
  sqrt = list(radiance, "planar>zigzag>zstd", "float32",
              list(error = "SQRT:MAX_ERROR=0.5N,A=25,B=2")),
  sentinel = list(counts, "planar>zigzag>zstd", "uint16", list(nodata = 200)),
  nan = list(holes, "planar>zigzag>zstd", "float32", list())
)

test_that("frames match the GeoZL Python package", {
  for (name in names(cases)) {
    case <- cases[[name]]
    expected <- readBin(testthat::test_path("fixtures", paste0("geozl-", name, ".frame")),
                        "raw", n = 1e6)
    graph <- do.call(rumi_geozl_graph,
                     c(list(case[[1]], case[[2]], datatype = case[[3]]), case[[4]]))
    expect_identical(rumi_geozl_compress(case[[1]], graph), expected, label = name)
  }
})

test_that("compressed frames write and read back", {
  frames <- rumi_frames(counts, "b (row h) (col w) -> row col (b h w)",
                        tile_size = 16, datatype = "uint16")
  for (i in seq_along(frames$data)) {
    graph <- rumi_geozl_graph(frames$data[[i]], "planar>zigzag>pfor",
                              datatype = frames$datatype)
    frames$compressed[[i]] <- rumi_geozl_compress(frames$data[[i]], graph)
  }
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  header <- rumi_write(path, frames, bands = c("B02", "B03", "B04"),
                       time = "2026-01-01")
  back <- rumi_read(path, header)
  expect_identical(dim(back), dim(counts))
  expect_equal(as.vector(back), as.vector(counts))

  bounded <- rumi_frames(array(reflectance, c(40, 48, 1)),
                         "b (row h) (col w) -> row col b (h w)",
                         tile_size = 16, datatype = "float32")
  for (i in seq_along(bounded$data)) {
    graph <- rumi_geozl_graph(bounded$data[[i]], "planar>zigzag>zstd",
                              error = 0.01, datatype = "FLT4S")
    bounded$compressed[[i]] <- rumi_geozl_compress(bounded$data[[i]], graph)
  }
  header <- rumi_write(path, bounded, bands = "B04", time = "2026-01-01")
  back <- rumi_read(path, header)
  expect_lte(max(abs(as.vector(back) - as.vector(reflectance))), 0.01 + 1e-7)
})

test_that("a graph prints its recipe and keeps its geometry", {
  graph <- rumi_geozl_graph(counts, "planar>zigzag>zstd", datatype = "UInt16")
  expect_s3_class(graph, "rumi_geozl_graph")
  expect_identical(graph$datatype, "uint16")
  expect_equal(c(graph$width, graph$planes), c(40, 3))
  expect_output(print(graph), "planar>zigzag>zstd, uint16, row width 40, 3 planes, lossless")
  expect_equal(rumi_geozl_graph(as.raw(1:12), "planar>zigzag>zstd",
                                width = 4)$planes, 1)
})

test_that("the core refuses what GeoZL would misread", {
  graph <- rumi_geozl_graph(counts, "planar>zigzag>zstd", datatype = "uint16")
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", error = -1,
                                datatype = "uint16"), "must not be negative")
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", error = "100%",
                                datatype = "uint16"), "between 0% and 100%")
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", planes = 7,
                                datatype = "uint16"), "whole rows")
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", nodata = 0,
                                datatype = "float16"), "no nodata sentinel")
  expect_error(rumi_geozl_graph(c(TRUE, FALSE, TRUE, TRUE), "planar>zigzag>zstd",
                                width = 2, nodata = 1), "nodata needs")
  expect_error(rumi_geozl_graph(as.raw(1:32), "planar>zigzag>zstd", width = 2,
                                datatype = "complex128"), "1, 2, 4 or 8 bytes")
  expect_error(rumi_geozl_graph(counts, "no such graph", datatype = "uint16"),
               "could not build the graph")
})

test_that("conversion refuses values a datatype cannot hold", {
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", error = TRUE),
               "must not be a logical")
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", datatype = "uint8"),
               "does not fit")
  expect_error(rumi_geozl_graph(c(1, NA), "planar>zigzag>zstd", width = 2,
                                datatype = "int16"), "needs `nodata`")
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", nodata = NA,
                                datatype = "uint16"), "cannot use NaN")
  expect_error(rumi_geozl_graph(1:10, "planar>zigzag>zstd"), "give `width`")
  expect_error(rumi_geozl_graph(counts, "planar>zigzag>zstd", datatype = "uint12"),
               "not a Rumi sample type")
  expect_error(rumi_geozl_compress(counts, list()), "must come from rumi_geozl_graph")
})
