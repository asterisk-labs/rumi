library(rumi)

# An Image of 5 columns, 3 rows and 2 bands, as R holds it, (x, y, band).
image <- array(seq_len(5 * 3 * 2) - 1L, c(5, 3, 2))

# The samples of one frame, without the datatype rumi_frames() attaches.
samples <- function(frames, i) structure(frames$data[[i]], rumi_datatype = NULL)

test_that("frames cut one band of one tile, clipped at the edges", {
  frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                        tile_size = 2, datatype = "uint16")
  expect_s3_class(frames, "rumi_frames")
  expect_identical(frames$layout, "h w")
  expect_length(frames$data, 12)
  expect_length(frames$compressed, 12)
  expect_identical(samples(frames, 1), image[1:2, 1:2, 1])
  expect_identical(samples(frames, 2), image[1:2, 1:2, 2])
  expect_identical(samples(frames, 5), array(image[5, 1:2, 1], c(1, 2)))
  expect_identical(samples(frames, 12), array(image[5, 3, 2], c(1, 1)))
  expect_identical(attr(frames$data[[12]], "rumi_datatype"), "uint16")
  expect_match(paste(capture.output(print(frames)), collapse = ""),
               "12 frames of h w, 5 x 3 in tiles of 2, uint16, 0 compressed")
})

test_that("cell frames keep every band, with the planes last", {
  frames <- rumi_frames(image, "b (row h) (col w) -> row col (b h w)",
                        tile_size = 2)
  expect_identical(frames$layout, "b h w")
  expect_identical(frames$datatype, "int32")
  expect_length(frames$data, 6)
  expect_identical(samples(frames, 1), image[1:2, 1:2, ])
  expect_identical(dim(frames$data[[6]]), c(1L, 1L, 2L))
})

test_that("write records the layout, bands, time and georeferencing", {
  frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                        tile_size = 2, datatype = "uint16")
  frames$compressed <- rep(list(as.raw(1:4)), length(frames$data))
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  header <- rumi_write(path, frames, bands = c("B04", "B08"),
                       time = "2025-08-08",
                       transform = c(10, 0, 739760, 0, -10, 5087560),
                       crs = "EPSG:32631")
  expect_type(header, "raw")

  file <- rumi_open(path, header)
  on.exit(rumi_close(file), add = TRUE)
  info <- rumi_info(file)
  expect_equal(info$shape, c(2, 3, 5))
  expect_identical(info$dtype, "uint16")
  expect_identical(info$tile, c(2L, 2L))
  expect_identical(info$frame_layout, "h w")
  expect_identical(info$bands, c("B04", "B08"))
  expect_equal(info$crs, 32631)
  expect_equal(info$transform, c(10, 0, 739760, 0, -10, 5087560))
  expect_equal(info$time, as.POSIXct("2025-08-08", tz = "UTC"))
})

test_that("a Cube takes instants or intervals, one per step", {
  cube <- array(0L, c(4, 4, 1, 2))
  frames <- rumi_frames(cube, "t b (row h) (col w) -> row col t b (h w)",
                        tile_size = 4, datatype = "uint8")
  frames$compressed <- rep(list(as.raw(1)), length(frames$data))
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))

  header <- rumi_write(path, frames, bands = "red",
                       time = as.Date(c("2025-08-08", "2025-08-13")))
  file <- rumi_open(path, header)
  expect_identical(rumi_info(file)$time_kind, "instant")
  rumi_close(file)

  steps <- list(c("2025-08-01", "2025-08-08"),
                c("2025-08-08T00:00:00", "2025-08-15T14:00:00+02:00"))
  header <- rumi_write(path, frames, bands = "red", time = steps)
  file <- rumi_open(path, header)
  on.exit(rumi_close(file), add = TRUE)
  info <- rumi_info(file)
  expect_identical(info$time_kind, "interval")
  expect_equal(unname(info$time[2, 2]), as.POSIXct("2025-08-15 12:00:00", tz = "UTC"))
})

test_that("frames and write refuse bad arguments", {
  expect_error(rumi_frames(1:10, "b (row h) (col w) -> row col b (h w)"),
               "must be an array")
  expect_error(rumi_frames(image, "(row h) (col w) -> row col (h w)"),
               "names 2 axes")
  expect_error(rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                           tile_size = 0), "tile_size must be at least 1")

  frames <- rumi_frames(image, "b (row h) (col w) -> row col b (h w)",
                        tile_size = 2, datatype = "uint16")
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
  frames$datatype <- "uint12"
  expect_error(rumi_write(path, frames, bands = c("B04", "B08"),
                          time = "2025-08-08"),
               "not a Rumi sample type")
  expect_false(file.exists(path))
})
