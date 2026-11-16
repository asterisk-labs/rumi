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
# Exercises float16 ties, subnormals and NaNs.
half <- local({
  g <- grid(40, 48)
  v <- ifelse(g$y == 0, 2048 + g$x %% 8,
              ifelse(g$y == 1, (g$x - 20) / 3 * 2^-14,
                     ((g$y * 131 + g$x * 71) %% 1000) / 7 - 60))
  v[(g$y * 40 + g$x) %% 89 == 0] <- NaN
  matrix(v, 40, 48)
})
# int64 samples below 2^53, with the most negative int64 as the sentinel.
wide <- local({
  g <- grid(40, 48)
  v <- (g$x * 7919 + g$y * 104729) %% 1000 * 1e12 - 5e14
  v[(g$y * 40 + g$x) %% 83 == 0] <- -2^63
  matrix(v, 40, 48)
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
  nan = list(holes, "planar>zigzag>zstd", "float32", list()),
  half = list(half, "planar>zigzag>zstd", "float16", list()),
  int64 = list(wide, "planar>zigzag>zstd", "int64", list(nodata = -2^63))
)

test_that("frames match the GeoZL Python package", {
  for (name in names(cases)) {
    case <- cases[[name]]
    expected <- readBin(testthat::test_path("fixtures", paste0("geozl-", name, ".frame")),
                        "raw", n = 1e6)
    graph <- do.call(rumi_geozl_graph,
                     c(list(case[[1]], case[[2]], dtype = case[[3]]), case[[4]]))
    expect_identical(rumi_geozl_compress(case[[1]], graph), expected, label = name)
  }
})

test_that("compressed frames write and read back", {
  frames <- rumi_frames(counts, "b (row h) (col w) -> row col (b h w)",
                        tile_size = 16, dtype = "uint16")
  for (i in seq_along(frames$data)) {
    graph <- rumi_geozl_graph(frames$data[[i]], "planar>zigzag>pfor",
                              dtype = frames$dtype)
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
                         tile_size = 16, dtype = "float32")
  for (i in seq_along(bounded$data)) {
    graph <- rumi_geozl_graph(bounded$data[[i]], "planar>zigzag>zstd",
                              error = 0.01, dtype = "FLT4S")
    bounded$compressed[[i]] <- rumi_geozl_compress(bounded$data[[i]], graph)
  }
  header <- rumi_write(path, bounded, bands = "B04", time = "2026-01-01")
  back <- rumi_read(path, header)
  expect_lte(max(abs(as.vector(back) - as.vector(reflectance))), 0.01 + 1e-7)
})

test_that("a frame compresses as the dtype rumi_frames() gave it", {
  frames <- rumi_frames(counts, "b (row h) (col w) -> row col (b h w)",
                        tile_size = 16, dtype = "uint16")
  for (i in seq_along(frames$data)) {
    graph <- rumi_geozl_graph(frames$data[[i]], "planar>zigzag>pfor")
    expect_identical(graph$dtype, "uint16")
    frames$compressed[[i]] <- rumi_geozl_compress(frames$data[[i]], graph)
  }
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  header <- rumi_write(path, frames, bands = c("B02", "B03", "B04"),
                       time = "2026-01-01")
  expect_equal(as.vector(rumi_read(path, header)), as.vector(counts))

  tile <- frames$data[[1]]
  expect_identical(rumi_geozl_graph(tile, "planar>zigzag>zstd",
                                    dtype = "UInt16")$dtype, "uint16")
  expect_error(rumi_geozl_graph(tile, "planar>zigzag>zstd", dtype = "int32"),
               "not the one rumi_frames\\(\\) gave this frame")
  other <- rumi_geozl_graph(as.vector(tile), "planar>zigzag>zstd", width = 16,
                            dtype = "int32")
  expect_error(rumi_geozl_compress(tile, other), "dtype is not the graph's")
})

test_that("a graph prints its recipe and keeps its geometry", {
  graph <- rumi_geozl_graph(counts, "planar>zigzag>zstd", dtype = "UInt16")
  expect_s3_class(graph, "rumi_geozl_graph")
  expect_named(graph, c("ptr", "method", "width", "planes", "dtype", "itemsize",
                        "error", "nodata"))
  expect_identical(graph$dtype, "uint16")
  expect_identical(graph$itemsize, 2)
  expect_equal(c(graph$width, graph$planes), c(40, 3))
  expect_output(print(graph), paste("<rumi_geozl_graph> planar>zigzag>zstd, uint16,",
                                    "row width 40, 3 planes, lossless"), fixed = TRUE)
  one <- rumi_geozl_graph(matrix(1, 48, 4), "planar>zigzag>pfor", dtype = "UInt16")
  expect_output(print(one), paste("<rumi_geozl_graph> planar>zigzag>pfor, uint16,",
                                  "row width 48, 1 plane, lossless"), fixed = TRUE)
  zero <- rumi_geozl_graph(counts, "planar>zigzag>zstd", error = 0, dtype = "uint16")
  expect_null(zero$error)
  expect_output(print(zero), "lossless")
  bounded <- rumi_geozl_graph(reflectance, "planar>zigzag>zstd", error = 0.01,
                              dtype = "float32")
  expect_identical(bounded$error, "LINEAR:MAX_ERROR=0.01")
  expect_identical(bounded$itemsize, 4)
  expect_identical(rumi_geozl_graph(reflectance, "planar>zigzag>zstd", error = "1%",
                                    dtype = "float32")$error, "LOG:MAX_ERROR=1%")
  for (zero_error in list("0%", "LINEAR:MAX_ERROR=0", "LOG:MAX_ERROR=0.0%"))
    expect_null(rumi_geozl_graph(reflectance, "planar>zigzag>zstd", error = zero_error,
                                 dtype = "float32")$error)
  expect_equal(rumi_geozl_graph(as.raw(1:12), "planar>zigzag>zstd",
                                width = 4)$planes, 1)
})

# The condition an expression raises, or NULL.
refusal <- function(expr) tryCatch({
  expr
  NULL
}, error = identity)

# An expected refusal and its text. The promise runs inside refusal().
case <- function(expr, message) list(refusal(expr), message)

# The texts below mirror the geozl R package, with rumi_geozl_ for geozl_. R
# checks only what the core cannot see.
test_that("R's argument checks use the geozl R package's words, without a call", {
  graph <- function(...) rumi_geozl_graph(counts, "planar>zigzag>zstd", ...)
  cases <- list(
    case(rumi_geozl_graph(counts, "", dtype = "uint16"),
         "`method` must be a recipe such as \"planar>zigzag>pfor\""),
    case(rumi_geozl_graph(counts, NA_character_, dtype = "uint16"),
         "`method` must be a recipe such as \"planar>zigzag>pfor\""),
    case(graph(error = TRUE), "`error` must not be a logical"),
    case(graph(error = c(1, 2)), "`error` must be a single number, percentage or recipe"),
    case(graph(error = NA_real_), "`error` must be a single number, percentage or recipe"),
    case(graph(error = list(1)), "`error` must be a number, percentage or recipe"),
    case(graph(error = "5"), paste0("`error` must be a number, a percentage such as ",
                                    "\"10%\", or a LINEAR, LOG or SQRT recipe, got \"5\"")),
    case(graph(error = "ten"), paste0("`error` must be a number, a percentage such as ",
                                      "\"10%\", or a LINEAR, LOG or SQRT recipe, got \"ten\"")),
    case(graph(error = ""), paste0("`error` must be a number, a percentage such as ",
                                   "\"10%\", or a LINEAR, LOG or SQRT recipe, got \"\"")),
    case(graph(width = 0, dtype = "uint16"), "`width` must be a whole number of at least 1"),
    case(graph(width = 3.7, dtype = "uint16"), "`width` must be a whole number of at least 1"),
    case(graph(planes = NA, dtype = "uint16"), "`planes` must be a whole number of at least 1"),
    case(graph(width = 2^32, planes = 1, dtype = "uint16"),
         "`width` must be a whole number from 1 to 4294967295"),
    case(rumi_geozl_graph(1:10, "planar>zigzag>zstd"), "give `width` for a vector without dim"),
    case(graph(nodata = c(1, 2), dtype = "uint16"), "`nodata` must be a single number or NA"),
    case(graph(nodata = "1", dtype = "uint16"), "`nodata` must be a single number or NA"),
    case(graph(nodata = NaN, dtype = "uint16"), "a uint16 raster cannot use NaN as nodata"),
    case(graph(nodata = NA, dtype = "int8"), "a int8 raster cannot use NaN as nodata"),
    case(graph(nodata = 70000, dtype = "uint16"), "nodata 70000 does not fit uint16"),
    case(graph(nodata = 0.5, dtype = "int16"), "nodata 0.5 does not fit int16"),
    case(graph(nodata = 1e39, dtype = "float32"),
         sprintf("nodata %.17g does not fit float32", 1e39)),
    case(graph(dtype = 16L), "`dtype` must be a single string"),
    case(rumi_geozl_compress(counts, list()), "`graph` must come from rumi_geozl_graph()"),
    case(rumi_geozl_compress(counts, structure(list(ptr = NULL, dtype = "uint16"),
                                               class = "rumi_geozl_graph")),
         "`graph` must come from rumi_geozl_graph()"),
    case(rumi_geozl_compress(counts, unserialize(serialize(graph(dtype = "uint16"), NULL))),
         paste("this graph is no longer open; a saved graph does not survive a",
               "new session, build it again with rumi_geozl_graph()"))
  )
  for (case in cases) {
    e <- case[[1]]
    expect_s3_class(e, "simpleError")
    expect_identical(conditionMessage(e), case[[2]])
    expect_null(conditionCall(e), label = case[[2]])
  }
  unknown <- refusal(graph(dtype = "uint12"))
  expect_match(conditionMessage(unknown), paste0(
    "^unknown dtype \"uint12\"; use one of uint8, .*, or terra's \\(\"INT2U\"\\) ",
    "or GDAL's \\(\"UInt16\"\\) names$"))
  expect_null(conditionCall(unknown))
})

test_that("the core checks the error text, geometry and dtype as an invalid request", {
  graph <- function(...) rumi_geozl_graph(counts, "planar>zigzag>zstd", ...)
  halves <- as.raw(seq_len(64))
  cases <- list(
    case(graph(error = -1), "error must not be negative, got -1"),
    case(graph(error = Inf), "error must be finite, got Inf"),
    case(graph(error = "ten%"), "relative error must look like \"10%\", got \"ten%\""),
    case(graph(error = "150%"), "relative error must be between 0% and 100%, got \"150%\""),
    case(rumi_geozl_graph(as.raw(0:255), "id>zstd", width = 16, planes = 3),
         "3 planes do not split 256 elements into whole rows of 16"),
    case(rumi_geozl_graph(halves, "id>zstd", width = 4, error = 0.5, dtype = "bfloat16"),
         "the quantizers do not support dtype bfloat16"),
    case(rumi_geozl_graph(halves, "id>zstd", width = 4, nodata = 0, dtype = "bfloat16"),
         "nodata needs a dtype geozl knows, got bfloat16"),
    case(rumi_geozl_graph(c(TRUE, FALSE, TRUE, TRUE), "id>zstd", width = 2, nodata = 1),
         "nodata needs a dtype geozl knows, got bool"),
    case(rumi_geozl_graph(matrix(1, 8, 8), "id>zstd", nodata = -1, dtype = "float16"),
         "a half float carries no sentinel, NaN still works on one"),
    case(rumi_geozl_graph(as.raw(1:32), "id>zstd", width = 2, dtype = "complex128"),
         "dtype complex128 is 16 bytes per element, an OpenZL numeric stream is 1, 2, 4 or 8")
  )
  for (case in cases) {
    e <- case[[1]]
    expect_s3_class(e, "simpleError")
    expect_false(inherits(e, "rumi_error"), label = case[[2]])
    expect_identical(conditionMessage(e), case[[2]])
    expect_null(conditionCall(e), label = case[[2]])
  }
  expect_identical(graph(error = 5, dtype = "uint16")$error, "LINEAR:MAX_ERROR=5")
  g <- graph(dtype = "uint16")
  bytes <- rumi:::.pack(counts, list(name = "uint16"), NULL)
  mismatch <- refusal(rumi:::.rumi_call(rumi:::rumi_r_geozl_compress, g$ptr, bytes, "int16"))
  expect_identical(conditionMessage(mismatch), "this graph was built for dtype uint16, got int16")
})

test_that("library refusals name the call and the method", {
  invalid <- refusal(rumi_geozl_graph(counts, "no such graph", dtype = "uint16"))
  expect_s3_class(invalid, "simpleError")
  expect_false(inherits(invalid, "rumi_error"))
  expect_match(conditionMessage(invalid),
               "^GeoZL graph failed \\(method = \"no such graph\"\\): .*\\(ZL error code [0-9]+\\)$")
  expect_null(conditionCall(invalid))

  g <- rumi_geozl_graph(reflectance, "planar>zigzag>transpose>entropy", error = 0.5,
                        dtype = "float32")
  outside <- refusal(rumi_geozl_compress(reflectance * 1e6, g))
  expect_match(conditionMessage(outside),
               "^GeoZL compress failed \\(method = \"planar>zigzag>transpose>entropy\"\\): ")
  expect_null(conditionCall(outside))

  mask <- rumi_geozl_graph(c(TRUE, FALSE, TRUE, TRUE), "planar>zigzag>zstd", width = 2)
  expect_error(rumi_geozl_compress(as.raw(c(0, 1, 2, 0)), mask), "other than 0 or 1")
})

test_that("conversion refuses values a dtype cannot hold", {
  open <- function(x, dtype, ...)
    rumi_geozl_graph(x, "planar>zigzag>zstd", width = length(x), dtype = dtype, ...)
  cases <- list(
    case(open(c(1, 70000), "uint16"), "value 70000 at position 2 does not fit uint16"),
    case(open(c(1, 2.5), "int32"), "value 2.5 at position 2 does not fit int32"),
    case(open(c(1, 1e39), "float32"),
         sprintf("value %.17g at position 2 is outside the float32 range", 1e39)),
    case(open(c(1, 65520), "float16"), "value 65520 at position 2 is outside the float16 range"),
    case(open(c(1, NA), "int16"),
         "`x` has NA at position 2; an integer dtype needs `nodata` to mark it"),
    case(open(as.raw(1:3), "uint16"), "a raw vector of uint16 samples needs a length divisible by 2"),
    case(open(c(1i, 2i), NULL), "`x` must be a double, integer, logical or raw vector, not complex"),
    case(open(c(TRUE, NA), NULL), "`x` has NA at position 2, which a bool dtype cannot hold")
  )
  for (case in cases) {
    expect_identical(conditionMessage(case[[1]]), case[[2]])
    expect_null(conditionCall(case[[1]]), label = case[[2]])
  }
  expect_s3_class(open(c(65504, -65504), "float16"), "rumi_geozl_graph")
  expect_s3_class(open(c(-2^63, 0), "int64"), "rumi_geozl_graph")
  expect_error(open(c(1, 2^63), "int64"), "does not fit int64")
  expect_error(open(c(1, 2^64), "uint64"), "does not fit uint64")
  expect_error(open(c(1, -1), "uint64"), "does not fit uint64")
  # NA in x takes the sentinel, so it compresses as the sentinel itself.
  g <- open(c(1, NA, 3), "int16", nodata = -1)
  expect_identical(rumi_geozl_compress(c(1, NA, 3), g), rumi_geozl_compress(c(1, -1, 3), g))
})
