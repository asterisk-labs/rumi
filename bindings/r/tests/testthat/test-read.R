library(rumi)

path <- Sys.getenv(
  "RUMI_TEST_FILE",
  testthat::test_path("fixtures", "binding.rumi")
)
header_path <- Sys.getenv(
  "RUMI_TEST_HEADER",
  testthat::test_path("fixtures", "binding.header")
)
float_path <- testthat::test_path("fixtures", "binding-f64.rumi")
float_header_path <- testthat::test_path("fixtures", "binding-f64.header")

test_that("whole UTC days read as Date and other instants as POSIXct", {
  days <- rumi:::.metadata(list(
    time = c(1767225600, 1767571200, 1767657600, 1768003200),
    time_kind = "interval"
  ))
  expect_s3_class(days$time, "Date")
  expect_true(is.matrix(days$time))
  expect_identical(dim(days$time), c(2L, 2L))
  expect_identical(colnames(days$time), c("start", "end"))
  expect_equal(unname(days$time[1, ]), as.Date(c("2026-01-01", "2026-01-05")))
  expect_match(paste(capture.output(print(days)), collapse = "\n"),
               "2026-01-01/2026-01-05")

  clock <- rumi:::.metadata(list(
    time = c(1767225600, 1767268800, 1767657600, 1768003200),
    time_kind = "interval"
  ))
  expect_s3_class(clock$time, "POSIXct")
  expect_true(is.matrix(clock$time))
  expect_equal(unname(clock$time[1, 2]),
               as.POSIXct("2026-01-01 12:00:00", tz = "UTC"))
  expect_match(paste(capture.output(print(clock)), collapse = "\n"),
               "2026-01-01 UTC/2026-01-01 12:00:00 UTC")

  instants <- rumi:::.metadata(list(time = c(0, 43200), time_kind = "instant"))
  expect_s3_class(instants$time, "POSIXct")
  expect_false(is.matrix(instants$time))
})

test_that("a file with time outside the years 1 to 9999 is refused", {
  cube <- array(0L, c(4, 4, 1, 2))
  frames <- rumi_frames(cube, "t b (row h) (col w) -> row col t b (h w)",
                        tile_size = 4, dtype = "uint8")
  for (i in seq_along(frames$data)) {
    graph <- rumi_geozl_graph(frames$data[[i]], "planar>zigzag>zstd")
    frames$compressed[[i]] <- rumi_geozl_compress(frames$data[[i]], graph)
  }
  path <- tempfile(fileext = ".rumi")
  on.exit(unlink(path))
  header <- rumi_write(path, frames, bands = "red",
                       time = as.Date(c("2025-08-08", "2025-08-13")))
  bytes <- readBin(path, "raw", file.info(path)$size)
  # The trailer's time fields, as SPEC.md lays them out. Days 20308 and 20313
  # lie on a line, so time_bits is 0 and time_step alone sets the 2nd step.
  le <- function(x, n) writeBin(as.integer(c(x, 0)), raw(), size = 4,
                                endian = "little")[seq_len(n)]
  fields <- c(as.raw(2), as.raw(0), le(20308, 8), le(5, 8), le(86400, 4))
  at <- which(vapply(seq_len(length(bytes) - length(fields) + 1), function(i)
    identical(bytes[i:(i + length(fields) - 1L)], fields), logical(1)))
  expect_length(at, 1)
  bytes[at + 10:17] <- le(3000000, 8)

  late <- expect_error(rumi_info(bytes), class = "rumi_error_format")
  expect_identical(conditionMessage(late),
                   "the 2nd time step falls outside the years 1 to 9999")
  expect_identical(late$kind, "format")
  expect_null(conditionCall(late))
  expect_error(rumi_info(bytes, header), "the 2nd time step falls outside",
               class = "rumi_error_format")

  expect_identical(rumi:::.ordinal(c(1)), "1st")
  expect_identical(vapply(c(2, 3, 4, 11, 12, 13, 21, 112), rumi:::.ordinal, ""),
                   c("2nd", "3rd", "4th", "11th", "12th", "13th", "21st", "112th"))
  interval <- tryCatch(rumi:::.metadata(list(
    time = c(0, 86400, 86400, 3e11, 3e11, 3.1e11), time_kind = "interval"
  )), error = identity)
  expect_identical(conditionMessage(interval),
                   "the 2nd time step falls outside the years 1 to 9999")
})

test_that("fixture files are installed", {
  expect_true(file.exists(path))
  expect_true(file.exists(header_path))
  expect_true(file.exists(float_path))
  expect_true(file.exists(float_header_path))
})

if (file.exists(path) && file.exists(header_path)) {
  test_that("info checks a pair and exposes all metadata", {
    info <- rumi_info(path, readBin(header_path, "raw", file.info(header_path)$size))
    expect_named(info, c("header", "shape", "time_count", "dtype", "tile",
                         "frame_layout", "index_order", "frames", "bands",
                         "time", "time_kind", "transform", "crs",
                         "pixel_is_point"))
    expect_equal(info$shape, c(2, 3, 6, 8))
    expect_identical(info$dtype, "uint16")
    expect_identical(info$bands, c("B02", "B03", "B04"))
    expect_equal(info$crs, 32618)
    expect_equal(info$transform, c(10, 0, 500000, 0, -10, 8000000))
    expect_identical(info$time_kind, "instant")
    expect_identical(info$time, as.Date(c("2026-01-01", "2026-01-06")))
    expect_false(info$pixel_is_point)
  })

  test_that("info reads a source, a header or both", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    alone <- rumi_info(header = header)
    expect_s3_class(alone, "rumi_metadata")
    expect_equal(alone$shape, c(2, 3, 6, 8))
    expect_identical(alone$dtype, "uint16")
    expect_identical(alone$frame_layout, "h w")
    expect_identical(alone$header, header)
    for (field in c("bands", "time", "time_kind", "transform", "crs",
                    "pixel_is_point"))
      expect_null(alone[[field]], label = field)
    expect_no_error(capture.output(print(alone)))

    rebuilt <- rumi_info(path)
    expect_identical(rebuilt$header, header)
    expect_identical(rebuilt$bands, c("B02", "B03", "B04"))
    expect_identical(rumi_info(path, header)$bands, rebuilt$bands)

    other <- readBin(float_header_path, "raw", file.info(float_header_path)$size)
    expect_error(rumi_info(path, other), "does not match")
    expect_error(rumi_info(), "a source, a header, or both")
    expect_error(rumi_info(header = "header"), "must be a raw vector")
  })

  test_that("a raw vector of file bytes is a source", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    bytes <- readBin(path, "raw", file.info(path)$size)
    expected <- rumi_read(path, header)

    expect_identical(rumi_info(bytes)$header, header)
    expect_identical(rumi_info(bytes, header)$bands, c("B02", "B03", "B04"))
    expect_identical(rumi_read(bytes, header), expected)
    expect_identical(
      rumi_read(bytes, header, time = 2, bands = 3, window = c(2, 3, 3, 4)),
      rumi_read(path, header, 2, 3, c(2, 3, 3, 4))
    )
  })

  test_that("a read takes the header alone", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    bytes <- readBin(path, "raw", file.info(path)$size)
    once <- rumi_read(path, header, bands = 2)
    expect_equal(dim(once), c(8, 6, 1, 2))
    expect_identical(names(dimnames(once)), c("x", "y", "band", "time"))
    expect_null(dimnames(once)$band)
    expect_null(dimnames(once)$time)
    expect_identical(attr(once, "rumi_axes"), c("x", "y", "band", "time"))
    expect_null(attr(once, "rumi_metadata")$bands)
    expect_equal(attr(once, "rumi_metadata")$shape, c(2, 3, 6, 8))
    expect_identical(rumi_info(path, header)$bands[2], "B03")

    # Without the trailer, the pair cannot be checked but the frames still read.
    cut <- bytes[-length(bytes)]
    expect_error(rumi_info(cut, header), "trailer")
    expect_identical(rumi_read(cut, header, bands = 2), once)
    batch <- rumi_read_many(list(cut, path), list(header, header), bands = 2)
    expect_identical(as.vector(batch[, , , , 1]), as.vector(once))
    expect_null(dimnames(batch)$band)
  })

  test_that("a path expands the home directory", {
    skip_on_os("windows")
    old_home <- Sys.getenv("HOME", unset = NA_character_)
    on.exit(if (is.na(old_home)) Sys.unsetenv("HOME") else
      Sys.setenv(HOME = old_home))
    Sys.setenv(HOME = dirname(path))
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    home <- file.path("~", basename(path))
    expect_identical(rumi_read(home, header), rumi_read(path, header))
    expect_identical(rumi_info(home, header)$bands, c("B02", "B03", "B04"))
  })

  test_that("partial reads use native R dimensions", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    whole <- rumi_read(path, header)
    expect_equal(dim(whole), c(8, 6, 3, 2))
    expect_equal(whole[1, 1, 1, 1], 0)
    expect_equal(whole[8, 6, 3, 2], 12507)

    chip <- rumi_read(path, header, time = c(2, 1), bands = c(3, 1),
                      window = c(2, 3, 3, 4))
    expect_equal(dim(chip), c(4, 3, 2, 2))
    expect_equal(chip[1, 1, 1, 1], 12102)
    expect_equal(chip[4, 3, 2, 2], 305)
    info <- rumi_info(path, header)
    expect_identical(info$bands[c(3, 1)], c("B04", "B02"))
    expect_identical(info$time[c(2, 1)], as.Date(c("2026-01-06", "2026-01-01")))
    # time comes before bands when they are positional.
    expect_identical(rumi_read(path, header, c(2, 1), c(3, 1), c(2, 3, 3, 4)), chip)

    one <- rumi_read(path, header, time = 2, bands = 1,
                     window = c(1, 1, 1, 1))
    expect_equal(dim(one), c(1, 1, 1, 1))
    expect_equal(one[1, 1, 1, 1], 10000)
  })

  test_that("float64 reads write directly to an R double array", {
    header <- readBin(float_header_path, "raw", file.info(float_header_path)$size)
    chip <- rumi_read(float_path, header, time = 2, bands = 3,
                      window = c(2, 3, 3, 4))
    expect_type(chip, "double")
    expect_equal(dim(chip), c(4, 3, 1, 1))
    expect_equal(chip[1, 1, 1, 1], 1210.2)
  })

  test_that("read_many keeps an n dimension", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    headers <- list(header, header)
    batch <- rumi_read_many(
      list(path, path), headers,
      windows = list(c(1, 1, 2, 3), c(3, 4, 2, 3)),
      bands = 2
    )
    expect_equal(dim(batch), c(3, 2, 1, 2, 2))
    expect_equal(batch[1, 1, 1, 1, 1], 1000)
    expect_equal(batch[1, 1, 1, 1, 2], 1203)
    expect_identical(attr(batch, "rumi_axes"), c("x", "y", "band", "time", "n"))
    expect_identical(names(dimnames(batch))[5L], "n")

    rows <- rumi_read_many(list(path, path), headers,
                           windows = rbind(c(1, 1, 2, 3), c(3, 4, 2, 3)),
                           bands = 2)
    expect_identical(rows, batch)
    whole <- rumi_read_many(list(path, path), headers,
                            windows = list(NULL, c(1, 1, 6, 8)))
    expect_equal(dim(whole), c(8, 6, 3, 2, 2))
  })

  test_that("read_many reads paths and raw vectors with their headers", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    bytes <- readBin(path, "raw", file.info(path)$size)
    windows <- list(c(1, 1, 2, 3), c(3, 4, 2, 3))
    expected <- rumi_read_many(list(path, path), list(header, header),
                               windows = windows, bands = 2)

    batch <- rumi_read_many(list(path, bytes), list(header, header), windows,
                            bands = 2)
    expect_identical(dim(batch), dim(expected))
    expect_identical(as.vector(batch), as.vector(expected))
    expect_identical(attr(batch, "rumi_axes"), c("x", "y", "band", "time", "n"))
    expect_identical(rumi_read_many(c(path, path), list(header, header),
                                    windows, bands = 2), batch)
    expect_error(rumi_read_many(list(path, path)), "one raw header per source")
    expect_error(rumi_read_many(list(path, path), header),
                 "one raw header per source")
    expect_error(rumi_read_many(bytes, list(header)), "use list\\(source\\)")
    expect_error(rumi_read_many(list(path), list(header), windows = windows),
                 "one window per source")
  })

  test_that("invalid headers and selections fail", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    bad <- header
    bad[[1L]] <- as.raw(bitwXor(as.integer(bad[[1L]]), 1L))
    expect_error(rumi_read(path, bad), "magic")

    expect_error(rumi_read(path, header, bands = 0),
                 "bands selects a band before the first")
    expect_error(rumi_read(path, header, bands = integer(0)), "bands selects no band")
    expect_error(rumi_read(path, header, bands = matrix(c(1, 2), 1)),
                 "`bands` must be a vector of indices, not a matrix or array",
                 fixed = TRUE)
    expect_error(rumi_read(path, header, time = array(2, c(1, 1, 1))),
                 "`time` must be a vector of indices, not a matrix or array",
                 fixed = TRUE)
    expect_error(rumi_read_many(list(path), list(header), bands = matrix(1)),
                 "not a matrix")
    expect_error(rumi_read(path, header, time = 3), "time selects the 3rd time step")
    expect_error(rumi_read(path, header, window = c(6, 8, 2, 2)), "past")
    expect_error(rumi_read(path), "must be a raw vector")
    expect_error(rumi_read(1, header), "one non-empty path or a raw vector")
    shape <- expect_error(rumi_frame_start(c(70, 40), "b (row h) (col w) -> row col b (h w)"),
                          "names 3 axes")
    expect_null(conditionCall(shape))
    axes <- expect_error(rumi_frames(array(0L, c(5, 3, 2)), "(row h) (col w) -> row col (h w)"),
                         "names 2 axes")
    expect_null(conditionCall(axes))
  })

  test_that("native failures carry their kind", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    missing <- expect_error(rumi_info(file.path(tempdir(), "missing.rumi")),
                            class = "rumi_error_io")
    expect_s3_class(missing, c("rumi_error_io", "rumi_error", "error", "condition"),
                    exact = TRUE)
    expect_identical(missing$kind, "io")
    expect_null(conditionCall(missing))
    expect_match(conditionMessage(missing), "missing.rumi")

    bad <- header
    bad[[1L]] <- as.raw(bitwXor(as.integer(bad[[1L]]), 1L))
    parse <- expect_error(rumi_read(path, bad), "magic", class = "rumi_error_parse")
    expect_identical(parse$kind, "parse")

    bytes <- readBin(path, "raw", file.info(path)$size)
    format <- expect_error(rumi_info(bytes[1:100]), class = "rumi_error_format")
    expect_identical(format$kind, "format")

    # An invalid request stays a plain R error.
    invalid <- expect_error(rumi_read(path, header, bands = 0), "before the first")
    expect_false(inherits(invalid, "rumi_error"))
  })
}
