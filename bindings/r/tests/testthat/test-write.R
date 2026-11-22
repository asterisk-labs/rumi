library(rumi)

# An Image of 5 columns, 3 rows and 2 bands, as R holds it, (x, y, band).
image <- array(seq_len(5 * 3 * 2) - 1L, c(5, 3, 2))

# The samples of one frame, without the dtype rumi_frames() attaches.
samples <- function(frames, i) structure(frames$data[[i]], rumi_dtype = NULL)

test_that("frames cut one band of one tile, clipped at the edges", {
  frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                        tile_size = 2, dtype = "uint16")
  expect_s3_class(frames, "rumi_frames")
  expect_identical(frames$layout, "h w")
  expect_length(frames$data, 12)
  expect_length(frames$compressed, 12)
  expect_identical(samples(frames, 1), image[1:2, 1:2, 1])
  expect_identical(samples(frames, 2), image[1:2, 1:2, 2])
  expect_identical(samples(frames, 5), array(image[5, 1:2, 1], c(1, 2)))
  expect_identical(samples(frames, 12), array(image[5, 3, 2], c(1, 1)))
  expect_identical(attr(frames$data[[12]], "rumi_dtype"), "uint16")
  expect_identical(frames$dtype, "uint16")
  expect_match(paste(capture.output(print(frames)), collapse = ""),
               "12 frames of h w, 5 x 3 in tiles of 2, uint16, 0 compressed")
})

test_that("frames record their pattern, grid and one-based positions", {
  frames <- rumi_frames(image, " b (row h) (col w) -> row col b (h w) ",
                        tile_size = 2, dtype = "uint16")
  expect_identical(frames$pattern, "b (row h) (col w) -> row col b (h w)")
  expect_identical(frames$tiles_across, 3L)
  expect_identical(frames$tiles_down, 2L)
  expect_identical(frames$row, rep(1:2, each = 6))
  expect_identical(frames$col, rep(rep(1:3, each = 2), 2))
  expect_identical(frames$band, rep(1:2, 6))
  expect_null(frames$time)
  # The fifth frame is the first band of the clipped tile in the third column.
  expect_identical(samples(frames, 5), array(image[5, 1:2, 1], c(1, 2)))

  cell <- rumi_frames(image, "b (row h) (col w) -> row col (b h w)", tile_size = 2)
  expect_identical(cell$row, rep(1:2, each = 3))
  expect_identical(cell$col, rep(1:3, 2))
  expect_null(cell$band)
  expect_null(cell$time)

  cube <- array(0L, c(4, 4, 2, 3))
  steps <- rumi_frames(cube, "t b (row h) (col w) -> row col b t (h w)",
                       tile_size = 2, dtype = "uint8")
  expect_identical(steps$tiles_across, 2L)
  expect_identical(steps$band, rep(rep(1:2, each = 3), 4))
  expect_identical(steps$time, rep(1:3, 8))
  # A single band leaves the index walking time alone.
  held <- rumi_frames(array(0L, c(4, 4, 1, 3)),
                      "t b (row h) (col w) -> row col t b (h w)",
                      tile_size = 4, dtype = "uint8")
  expect_null(held$band)
  expect_identical(held$time, 1:3)
})

test_that("frame_start matches Python for the same arrays", {
  # Python's shapes, reversed into R's dim() order.
  expect_identical(rumi_frame_start(c(70, 40, 2),
                                    "b (row h) (col w) -> row col b (h w)", 16), 812)
  expect_identical(rumi_frame_start(c(70, 40, 2),
                                    "b (row h) (col w) -> row col (b h w)", 16), 632)
  expect_identical(rumi_frame_start(c(5, 70, 40),
                                    "(row h) (col w) b -> row col (b h w)", 16), 652)
  expect_identical(rumi_frame_start(c(70, 40, 3, 4),
                                    "t b (row h) (col w) -> row col (t b h w)", 16), 632)
  expect_identical(rumi_frame_start(c(70, 40, 3, 4),
                                    "t b (row h) (col w) -> row col b t (h w)", 16), 2612)
  expect_identical(rumi_frame_start(dim(image), "b (row h) (col w) -> row col b (h w)"),
                   rumi_frame_start(c(5, 3, 2), "b (row h) (col w) -> row col b (h w)",
                                    512L))
  expect_error(rumi_frame_start(c(70, 40), "b (row h) (col w) -> row col b (h w)", 16),
               "names 3 axes, but `shape` has 2 values")
  expect_error(rumi_frame_start(c(70, 40.5, 2), "b (row h) (col w) -> row col b (h w)"),
               "whole, non-negative")
  expect_error(rumi_frame_start(c(70, 40, 2), "b (row h) (col w) -> row col b (h w)", 0),
               "tile_size must be at least 1")
  expect_error(rumi_frame_start(c(70, 40, 2), "b (row h) (col w) -> row col b (h w)",
                                70000), "at most 65535")
})

test_that("cell frames keep every band, with the planes last", {
  frames <- rumi_frames(image, "b (row h) (col w) -> row col (b h w)",
                        tile_size = 2)
  expect_identical(frames$layout, "b h w")
  expect_identical(frames$dtype, "int32")
  expect_length(frames$data, 6)
  expect_identical(samples(frames, 1), image[1:2, 1:2, ])
  expect_identical(dim(frames$data[[6]]), c(1L, 1L, 2L))
})

test_that("write records the layout, bands, time and georeferencing", {
  frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                        tile_size = 2, dtype = "uint16")
  frames$compressed <- rep(list(as.raw(1:4)), length(frames$data))
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  header <- rumi_write(path, frames, bands = c("B04", "B08"),
                       time = "2025-08-08",
                       transform = c(10, 0, 739760, 0, -10, 5087560),
                       crs = "EPSG:32631")
  expect_type(header, "raw")

  info <- rumi_info(path, header)
  expect_equal(info$shape, c(2, 3, 5))
  expect_identical(info$dtype, "uint16")
  expect_identical(info$tile, c(2L, 2L))
  expect_identical(info$frame_layout, "h w")
  expect_identical(info$bands, c("B04", "B08"))
  expect_equal(info$crs, 32631)
  expect_equal(info$transform, c(10, 0, 739760, 0, -10, 5087560))
  expect_identical(info$time, as.Date("2025-08-08"))
})

test_that("crs takes EPSG codes past R's integer range and sf objects", {
  frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                        tile_size = 2, dtype = "uint16")
  frames$compressed <- rep(list(as.raw(1:4)), length(frames$data))
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  write <- function(crs) {
    rumi_write(path, frames, bands = c("B04", "B08"), time = "2025-08-08",
               transform = c(10, 0, 739760, 0, -10, 5087560), crs = crs)
  }
  # The core judges codes above R's integer range, so they reach it intact.
  expect_error(write(4294967295), "EPSG:4294967295 is not a projected")
  expect_error(write("EPSG:2147483648"), "EPSG:2147483648 is not a projected")
  for (crs in list(0, "EPSG:0", 4294967296, -1, 1.5, "UTM", TRUE))
    expect_error(write(crs), "must be an EPSG code")
  expect_equal(rumi_info(path, write("32631"))$crs, 32631)

  skip_if_not_installed("sf")
  expect_equal(rumi_info(path, write(sf::st_crs(32618)))$crs, 32618)
  expect_error(write(sf::st_crs(NA)), "must be an EPSG code")
})

test_that("a Cube takes instants or intervals, one per step", {
  cube <- array(0L, c(4, 4, 1, 2))
  frames <- rumi_frames(cube, "t b (row h) (col w) -> row col t b (h w)",
                        tile_size = 4, dtype = "uint8")
  for (i in seq_along(frames$data)) {
    graph <- rumi_geozl_graph(frames$data[[i]], "planar>zigzag>zstd")
    frames$compressed[[i]] <- rumi_geozl_compress(frames$data[[i]], graph)
  }
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))

  days <- as.Date(c("2025-08-08", "2025-08-13"))
  header <- rumi_write(path, frames, bands = "red", time = days)
  expect_identical(rumi_info(path, header)$time_kind, "instant")
  expect_identical(rumi_info(path, header)$time, days)

  clock <- as.POSIXct(c("2025-08-08 10:30:00", "2025-08-13 00:00:00"), tz = "UTC")
  header <- rumi_write(path, frames, bands = "red", time = clock)
  expect_equal(rumi_info(path, header)$time, clock)
  expect_s3_class(rumi_info(path, header)$time, "POSIXct")

  steps <- list(c("2025-08-01", "2025-08-08"),
                c("2025-08-08T00:00:00", "2025-08-15T14:00:00+02:00"))
  header <- rumi_write(path, frames, bands = "red", time = steps)
  info <- rumi_info(path, header)
  expect_identical(info$time_kind, "interval")
  expect_s3_class(info$time, "POSIXct")
  expect_equal(unname(info$time[2, ]),
               as.POSIXct(c("2025-08-08 00:00:00", "2025-08-15 12:00:00"), tz = "UTC"))
})

test_that("the writer refuses time outside the years 1 to 9999", {
  cube <- array(0L, c(4, 4, 1, 2))
  frames <- rumi_frames(cube, "t b (row h) (col w) -> row col t b (h w)",
                        tile_size = 4, dtype = "uint8")
  frames$compressed <- rep(list(as.raw(1)), length(frames$data))
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  late <- c(as.Date("2025-08-08"), as.Date("9999-12-31") + 1)
  expect_error(rumi_write(path, frames, bands = "red", time = late),
               "the 2nd time step falls outside the years 1 to 9999", fixed = TRUE)
  early <- as.POSIXct(c("2025-08-08", "0000-12-31"), tz = "UTC")
  expect_error(rumi_write(path, frames, bands = "red", time = early),
               "the 2nd time step falls outside the years 1 to 9999", fixed = TRUE)
  expect_false(file.exists(path))
})

test_that("frames and write refuse bad arguments", {
  expect_error(rumi_frames(1:10, "b (row h) (col w) -> row col b (h w)"),
               "must be an array")
  expect_error(rumi_frames(image, "(row h) (col w) -> row col (h w)"),
               "names 2 axes")
  expect_error(rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                           tile_size = 0), "tile_size must be at least 1")

  frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                        tile_size = 2, dtype = "uint16")
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = "2025-08-08"),
               "frame 1 has no compressed bytes")
  frames$compressed <- rep(list(as.raw(1)), length(frames$data))
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = "2025-08-08", crs = 32631),
               "given together")
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = c("2025-08-08", "2025-08-09")),
               "one entry per time step")
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = "8 August"),
               "not an ISO date")
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = "2025-08-08T10:00:00+25:00"),
               "not an ISO date")
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = as.POSIXct("2025-08-08 10:00:00.5", tz = "UTC")),
               "fraction of a second")
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = "2025-08-08",
                          transform = c(10, 0, 0, 0, -10, 0), crs = "UTM"),
               "EPSG code")
  expect_error(rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                           dtype = 16L), "`dtype` must be one sample type")
  frames$dtype <- "uint12"
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = "2025-08-08"),
               "not a Rumi sample type")
  expect_false(file.exists(path))
})

test_that("64-bit reads warn exactly when a value does not survive as double", {
  # The little-endian bytes of one 64-bit integer, hi * 2^32 + lo.
  le64 <- function(hi, lo) writeBin(as.integer(c(lo, hi)), raw(), size = 4,
                                    endian = "little")
  read_one <- function(bytes, dtype) {
    frames <- rumi_frames(array(0, c(1, 1, 1)), "b (row h) (col w) -> row col b (h w)",
                          dtype = dtype)
    graph <- rumi_geozl_graph(bytes, "planar>zigzag>zstd", width = 1, dtype = dtype)
    frames$compressed[[1]] <- rumi_geozl_compress(bytes, graph)
    path <- tempfile(fileext = ".rumi")
    on.exit(unlink(path))
    as.vector(rumi_read(path, rumi_write(path, frames, bands = "b", time = "2025-01-01")))
  }
  for (dtype in c("int64", "uint64")) {
    # 2^53 + 1 rounds to 2^53, while 2^53 + 2 is exact.
    expect_warning(back <- read_one(le64(2^21, 1), dtype), "lost precision",
                   label = dtype)
    expect_identical(back, 2^53)
    expect_no_warning(back <- read_one(le64(2^21, 2), dtype), message = dtype)
    expect_identical(back, 2^53 + 2)
  }
  # 2^63 - 1 and 2^64 - 1 round up past their type.
  expect_warning(read_one(le64(2^31 - 1, -1), "int64"), "lost precision")
  expect_warning(read_one(le64(-1, -1), "uint64"), "lost precision")
  expect_no_warning(back <- read_one(as.raw(c(0, 0, 0, 0, 0, 0, 0, 0x80)), "int64"))
  expect_identical(back, -2^63)
})
