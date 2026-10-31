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

test_that("interval metadata keeps POSIXct", {
  metadata <- rumi:::.metadata(list(
    time = c(1767225600, 1767571200, 1767657600, 1768003200),
    time_kind = "interval"
  ))
  expect_s3_class(metadata$time, "POSIXct")
  expect_true(is.matrix(metadata$time))
  expect_identical(dim(metadata$time), c(2L, 2L))
  expect_equal(unname(metadata$time[1, ]), as.POSIXct(
    c("2026-01-01", "2026-01-05"), tz = "UTC"
  ))
  expect_match(paste(capture.output(print(metadata)), collapse = "\n"),
               "2026-01-01.*/2026-01-05")
})

test_that("fixture files are installed", {
  expect_true(file.exists(path))
  expect_true(file.exists(header_path))
  expect_true(file.exists(float_path))
  expect_true(file.exists(float_header_path))
})

if (file.exists(path) && file.exists(header_path)) {
  test_that("open validates and exposes all metadata", {
    file <- rumi_open(path, readBin(header_path, "raw", file.info(header_path)$size))
    on.exit(rumi_close(file))
    info <- rumi_info(file)
    expect_equal(info$shape, c(2, 3, 6, 8))
    expect_identical(info$dtype, "uint16")
    expect_identical(info$bands, c("B02", "B03", "B04"))
    expect_equal(info$crs, 32618)
    expect_equal(info$transform, c(10, 0, 500000, 0, -10, 8000000))
    expect_identical(info$time_kind, "instant")
    expect_length(info$time, 2)
  })

  test_that("open expands the home directory", {
    skip_on_os("windows")
    old_home <- Sys.getenv("HOME", unset = NA_character_)
    on.exit(if (is.na(old_home)) Sys.unsetenv("HOME") else
      Sys.setenv(HOME = old_home))
    Sys.setenv(HOME = dirname(path))
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    file <- rumi_open(file.path("~", basename(path)), header)
    on.exit(rumi_close(file), add = TRUE)
    expect_identical(file$path, path)
  })

  test_that("partial reads use native R dimensions", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    file <- rumi_open(path, header)
    on.exit(rumi_close(file))
    whole <- rumi_read(file)
    expect_equal(dim(whole), c(8, 6, 3, 2))
    expect_equal(whole[1, 1, 1, 1], 0)
    expect_equal(whole[8, 6, 3, 2], 12507)

    chip <- rumi_read(file, time = c(2, 1), bands = c(3, 1),
                      window = c(2, 3, 3, 4))
    expect_equal(dim(chip), c(4, 3, 2, 2))
    expect_equal(chip[1, 1, 1, 1], 12102)
    expect_equal(chip[4, 3, 2, 2], 305)

    one <- rumi_read(file, time = 2, bands = 1,
                     window = c(1, 1, 1, 1))
    expect_equal(dim(one), c(1, 1, 1, 1))
    expect_equal(one[1, 1, 1, 1], 10000)
  })

  test_that("float64 reads write directly to an R double array", {
    header <- readBin(float_header_path, "raw", file.info(float_header_path)$size)
    file <- rumi_open(float_path, header)
    on.exit(rumi_close(file))
    chip <- rumi_read(file, time = 2, bands = 3,
                      window = c(2, 3, 3, 4))
    expect_type(chip, "double")
    expect_equal(dim(chip), c(4, 3, 1, 1))
    expect_equal(chip[1, 1, 1, 1], 1210.2)
  })

  test_that("read_many keeps a sample dimension", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    file <- rumi_open(path, header)
    on.exit(rumi_close(file))
    batch <- rumi_read_many(
      list(file, file),
      windows = list(c(1, 1, 2, 3), c(3, 4, 2, 3)),
      bands = 2
    )
    expect_equal(dim(batch), c(3, 2, 1, 2, 2))
    expect_equal(batch[1, 1, 1, 1, 1], 1000)
    expect_equal(batch[1, 1, 1, 1, 2], 1203)
  })

  test_that("invalid headers, selections and closed datasets fail", {
    header <- readBin(header_path, "raw", file.info(header_path)$size)
    bad <- header
    bad[[1L]] <- as.raw(bitwXor(as.integer(bad[[1L]]), 1L))
    expect_error(rumi_open(path, bad), "magic")

    file <- rumi_open(path, header)
    expect_error(rumi_read(file, bands = 0), "positive, one-based")
    expect_error(rumi_read_many(file), "use list\\(file\\)")
    expect_error(rumi_read(file, window = c(6, 8, 2, 2)), "past")
    rumi_close(file)
    expect_error(rumi_read(file), "closed")
    expect_no_error(rumi_close(file))
  })
}
