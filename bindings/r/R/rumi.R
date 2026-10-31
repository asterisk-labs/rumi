.rumi_call <- function(name, ...) {
  .Call(name, ..., PACKAGE = "rumi")
}

.header <- function(header) {
  if (!is.raw(header))
    stop("`header` must be a raw vector", call. = FALSE)
  header
}

.metadata <- function(value) {
  if (!is.null(value$time)) {
    time <- as.POSIXct(value$time, origin = "1970-01-01", tz = "UTC")
    if (identical(value$time_kind, "interval")) {
      index <- matrix(seq_along(time), ncol = 2L, byrow = TRUE)
      time <- time[as.vector(index)]
      dim(time) <- dim(index)
      dimnames(time) <- list(NULL, c("start", "end"))
    }
    value$time <- time
  }
  class(value) <- "rumi_metadata"
  value
}

#' Open a Rumi source
#'
#' `path` and its external `header` stay paired for every partial read.
#' Opening validates the header against the source and reads its metadata.
#'
#' @param path Local path, URI or VSI path.
#' @param header Raw external-header bytes.
#' @return A `rumi_dataset`.
#' @export
rumi_open <- function(path, header) {
  if (!is.character(path) || length(path) != 1L || is.na(path) || !nzchar(path))
    stop("`path` must be one non-empty string", call. = FALSE)
  value <- .rumi_call(rumi_r_open, enc2utf8(path.expand(path)), .header(header))
  value$metadata <- .metadata(value$metadata)
  class(value) <- "rumi_dataset"
  value
}

#' Close a Rumi dataset
#' @param file A dataset returned by [rumi_open()].
#' @export
rumi_close <- function(file) {
  .file(file)
  invisible(.rumi_call(rumi_r_close, file$pointer))
}

.file <- function(file) {
  if (!inherits(file, "rumi_dataset") || !is.list(file) ||
      !identical(names(file), c("pointer", "path", "header", "metadata")))
    stop("`file` must come from rumi_open()", call. = FALSE)
  file
}

#' Inspect a Rumi dataset
#' @param file A dataset returned by [rumi_open()].
#' @export
rumi_info <- function(file) .file(file)$metadata

.selection <- function(value, name) {
  if (is.null(value))
    return(NULL)
  if (!is.numeric(value) || !length(value) || anyNA(value) ||
      any(!is.finite(value)) || any(value != floor(value)))
    stop(sprintf("`%s` must contain whole, one-based indices", name),
         call. = FALSE)
  if (any(value < 1))
    stop(sprintf("`%s` must contain positive, one-based indices", name),
         call. = FALSE)
  if (any(value > .Machine$integer.max))
    stop(sprintf("`%s` indices are outside R's integer range", name),
         call. = FALSE)
  as.integer(value)
}

.window <- function(value) {
  if (is.null(value))
    return(NULL)
  if (!is.numeric(value) || length(value) != 4L || anyNA(value) ||
      any(!is.finite(value)) || any(value != floor(value)) || any(value < 1))
    stop("`window` must be (row, column, height, width), with one-based origins",
         call. = FALSE)
  if (any(value > .Machine$integer.max))
    stop("`window` values are outside R's integer range", call. = FALSE)
  as.double(value)
}

.pattern <- function(value) {
  if (is.null(value))
    return(NULL)
  if (!is.character(value) || length(value) != 1L || is.na(value) || !nzchar(value))
    stop("`pattern` must be one non-empty string or NULL", call. = FALSE)
  value
}

.decorate <- function(x, file, bands, time, batch = FALSE) {
  metadata <- file$metadata
  axes <- c("x", "y", "band")
  if (metadata$time_count > 1L)
    axes <- c(axes, "time")
  if (batch)
    axes <- c(axes, "sample")
  attr(x, "rumi_axes") <- axes
  attr(x, "rumi_metadata") <- metadata

  labels <- vector("list", length(dim(x)))
  names(labels) <- axes
  picked_bands <- if (is.null(bands)) seq_along(metadata$bands) else bands
  if (length(metadata$bands))
    labels$band <- metadata$bands[picked_bands]
  picked_time <- if (is.null(time)) seq_len(metadata$time_count) else time
  if (metadata$time_count > 1L && length(metadata$time)) {
    if (is.matrix(metadata$time)) {
      picked <- metadata$time[picked_time, , drop = FALSE]
      labels$time <- paste(
        format(picked[, 1L], tz = "UTC", usetz = TRUE),
        format(picked[, 2L], tz = "UTC", usetz = TRUE),
        sep = "/"
      )
    } else {
      labels$time <- format(metadata$time[picked_time], tz = "UTC", usetz = TRUE)
    }
  }
  dimnames(x) <- labels
  x
}

#' Read a partial Rumi array
#'
#' `bands`, `time` and window origins use R's one-based indexing. `window` is
#' `(row, column, height, width)`. The returned Image is `(X, Y, Band)` and a
#' Cube is `(X, Y, Band, Time)`, matching R's column-major memory order.
#'
#' @param file A dataset returned by [rumi_open()]. Alternatively, a path when
#'   `header` is supplied.
#' @param header External header when `file` is a path.
#' @param bands,time One-based indices, in output order, or `NULL` for all.
#' @param window One-based `(row, column, height, width)`, or `NULL` for all.
#' @param pattern Optional Rumi output pattern.
#' @export
rumi_read <- function(file, header = NULL, bands = NULL, time = NULL,
                      window = NULL, pattern = NULL) {
  owned <- is.character(file)
  if (owned) {
    file <- rumi_open(file, .header(header))
    on.exit(rumi_close(file), add = TRUE)
  } else if (!is.null(header)) {
    stop("`header` is already carried by this dataset", call. = FALSE)
  }
  file <- .file(file)
  bands <- .selection(bands, "bands")
  time <- .selection(time, "time")
  x <- .rumi_call(rumi_r_read, file$pointer, time, bands,
                  .window(window), .pattern(pattern))
  if (is.null(pattern))
    x <- .decorate(x, file, bands, time)
  x
}

#' Read one window from several Rumi datasets
#'
#' @param files A list of datasets from [rumi_open()].
#' @param windows A list or matrix with one `(row, column, height, width)` row
#'   per dataset. All windows have the same height and width.
#' @inheritParams rumi_read
#' @export
rumi_read_many <- function(files, windows = NULL, bands = NULL, time = NULL,
                           pattern = NULL) {
  if (inherits(files, "rumi_dataset"))
    stop("`files` must be a list of Rumi datasets; use list(file)",
         call. = FALSE)
  if (!is.list(files) || !length(files))
    stop("`files` must be a non-empty list of Rumi datasets", call. = FALSE)
  files <- lapply(files, .file)
  if (is.null(windows)) {
    windows <- vector("list", length(files))
  } else if (is.matrix(windows)) {
    windows <- lapply(seq_len(nrow(windows)), function(i) windows[i, ])
  }
  if (!is.list(windows) || length(windows) != length(files))
    stop("`windows` must have one window per dataset", call. = FALSE)
  windows <- lapply(windows, .window)
  bands <- .selection(bands, "bands")
  time <- .selection(time, "time")
  x <- .rumi_call(rumi_r_read_many, lapply(files, `[[`, "pointer"), windows,
                  time, bands, .pattern(pattern))
  if (is.null(pattern))
    x <- .decorate(x, files[[1L]], bands, time, batch = TRUE)
  x
}

#' @export
print.rumi_dataset <- function(x, ...) {
  cat("<Rumi dataset>\n")
  print(x$metadata)
  invisible(x)
}

#' @export
print.rumi_metadata <- function(x, ...) {
  cat("  shape:", paste(x$shape, collapse = " x "), "\n")
  cat("  dtype:", x$dtype, "\n")
  cat("  tile:", paste(x$tile, collapse = " x "), "\n")
  cat("  frame layout:", x$frame_layout, "\n")
  cat("  bands:", paste(x$bands, collapse = ", "), "\n")
  if (!is.null(x$crs))
    cat("  CRS: EPSG:", x$crs, "\n", sep = "")
  if (length(x$time)) {
    first <- if (is.matrix(x$time)) {
      paste(
        format(x$time[1L, 1L], tz = "UTC", usetz = TRUE),
        format(x$time[1L, 2L], tz = "UTC", usetz = TRUE),
        sep = "/"
      )
    } else {
      format(x$time[[1L]], tz = "UTC", usetz = TRUE)
    }
    cat("  time:", first, "...\n")
  }
  invisible(x)
}
