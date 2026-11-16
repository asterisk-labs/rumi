library(rumi)

path <- testthat::test_path("fixtures", "binding.rumi")
header_path <- testthat::test_path("fixtures", "binding.header")

test_that("settings validate their arguments", {
  expect_type(rumi_get_num_threads(), "integer")
  expect_type(rumi_get_checksum_verification(), "logical")
  for (n in list(2.5, NA, "2", c(1, 2), 2^31))
    expect_error(rumi_set_num_threads(n), "`n` must be one whole number")
  invalid <- expect_error(rumi_set_num_threads(0), "in \\[1, 1024\\]")
  expect_false(inherits(invalid, "rumi_error"))
  expect_error(rumi_set_num_threads(1025), "in \\[1, 1024\\]")
  for (on in list(NA, 1, "yes", c(TRUE, FALSE)))
    expect_error(rumi_set_checksum_verification(on), "`on` must be TRUE or FALSE")
})

test_that("the first parallel read and decode pin the settings", {
  header <- readBin(header_path, "raw", file.info(header_path)$size)
  # Earlier reads in this process may have pinned the pool already.
  tryCatch(rumi_set_num_threads(2L), rumi_error_state = function(e) NULL)
  rumi_read(path, header)

  n <- rumi_get_num_threads()
  expect_identical(withVisible(rumi_set_num_threads(n)),
                   list(value = n, visible = FALSE))
  pinned <- expect_error(rumi_set_num_threads(n + 1L), "pinned",
                         class = "rumi_error_state")
  expect_s3_class(pinned, c("rumi_error_state", "rumi_error", "error", "condition"),
                  exact = TRUE)
  expect_identical(pinned$kind, "state")
  expect_null(conditionCall(pinned))
  expect_identical(rumi_get_num_threads(), n)

  on <- rumi_get_checksum_verification()
  expect_identical(withVisible(rumi_set_checksum_verification(on)),
                   list(value = on, visible = FALSE))
  pinned <- expect_error(rumi_set_checksum_verification(!on), "pinned",
                         class = "rumi_error_state")
  expect_identical(pinned$kind, "state")
  expect_identical(rumi_get_checksum_verification(), on)
})
