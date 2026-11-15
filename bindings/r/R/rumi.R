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
  if (!is.numeric(value) || anyNA(value) ||
      any(!is.finite(value)) || any(value != floor(value)))
    stop(sprintf("`%s` must contain whole, one-based indices", name),
         call. = FALSE)
  if (any(abs(value) > .Machine$integer.max))
    stop(sprintf("`%s` indices are outside R's integer range", name),
         call. = FALSE)
  as.integer(value)
}

.window <- function(value) {
  if (is.null(value))
    return(NULL)
  if (!is.numeric(value) || length(value) != 4L || anyNA(value) ||
      any(!is.finite(value)) || any(value != floor(value)))
    stop("`window` must be (row, column, height, width), with one-based origins",
         call. = FALSE)
  if (any(abs(value) > .Machine$integer.max))
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

.datatype <- function(value, x) {
  if (is.null(value))
    return(switch(typeof(x), integer = "int32", double = "float64",
                  raw = "uint8", logical = "bool", complex = "complex128",
                  stop("`x` must be a raw, logical, integer, double or complex array",
                       call. = FALSE)))
  if (!is.character(value) || length(value) != 1L || is.na(value) || !nzchar(value))
    stop("`datatype` must be one sample type such as \"uint16\"", call. = FALSE)
  value
}

#' Cut an array into frames
#'
#' The pattern names axes in C order; R holds them in reverse. For example,
#' `"b (row h) (col w) -> row col b (h w)"` cuts an `(x, y, band)` array.
#' Edge frames are clipped. Each frame is an R array with reversed stored
#' axes, ready for GeoZL. Set every `compressed` entry before [rumi_write()].
#'
#' @param x An array of samples.
#' @param pattern A frame pattern.
#' @param tile_size Height and width of a square tile, from 1 to 65535.
#' @param datatype The sample type the payloads are compressed as, such as
#'   `"uint16"`. Defaults to `"int32"` for integer arrays, `"float64"` for
#'   double, `"uint8"` for raw and `"bool"` for logical.
#' @return A `rumi_frames` list with `data`, one array per frame in file
#'   order, `compressed`, a list of `NULL` to fill, and the layout of the file.
#' @export
rumi_frames <- function(x, pattern, tile_size = 512L, datatype = NULL) {
  if (is.null(dim(x)))
    stop("`x` must be an array", call. = FALSE)
  if (!is.character(pattern) || length(pattern) != 1L || is.na(pattern) ||
      !nzchar(pattern))
    stop("`pattern` must be one frame pattern", call. = FALSE)
  if (!is.numeric(tile_size) || length(tile_size) != 1L || is.na(tile_size) ||
      tile_size != floor(tile_size))
    stop("`tile_size` must be a whole number", call. = FALSE)
  if (tile_size < 0)
    stop("`tile_size` must not be negative", call. = FALSE)
  if (tile_size > 65535)
    stop("`tile_size` must be at most 65535", call. = FALSE)
  datatype <- .datatype(datatype, x)
  value <- .rumi_call(rumi_r_frames, x, enc2utf8(pattern), as.integer(tile_size))
  value$compressed <- vector("list", length(value$data))
  value$datatype <- datatype
  class(value) <- "rumi_frames"
  value
}

.iso_seconds <- function(text) {
  if (grepl("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", text)) {
    date <- as.Date(text, format = "%Y-%m-%d")
    if (!is.na(date))
      return(as.numeric(date) * 86400)
  }
  match <- regmatches(text, regexec(
    "^([0-9]{4}-[0-9]{2}-[0-9]{2})[T ]([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:[.,]([0-9]+))?)?([Zz]|[+-][0-9]{2}(?::?[0-9]{2})?)?$",
    text, perl = TRUE
  ))[[1L]]
  error <- sprintf("\"%s\" is not an ISO date or datetime", text)
  if (!length(match))
    stop(error, call. = FALSE)
  day <- as.Date(match[2L], format = "%Y-%m-%d")
  clock <- as.integer(match[3:5])
  clock[is.na(clock)] <- 0L
  if (is.na(day) || clock[1L] > 23L || clock[2L] > 59L || clock[3L] > 59L)
    stop(error, call. = FALSE)
  if (nzchar(match[6L]) && grepl("[1-9]", match[6L]))
    stop("a time coordinate carries a fraction of a second; rumi records whole seconds",
         call. = FALSE)
  offset <- 0L
  zone <- match[7L]
  if (nzchar(zone) && toupper(zone) != "Z") {
    digits <- gsub(":", "", substring(zone, 2L), fixed = TRUE)
    hour <- as.integer(substr(digits, 1L, 2L))
    minute <- if (nchar(digits) == 4L) as.integer(substr(digits, 3L, 4L)) else 0L
    if (hour > 23L || minute > 59L)
      stop(error, call. = FALSE)
    offset <- (hour * 60L + minute) * 60L
    if (startsWith(zone, "-"))
      offset <- -offset
  }
  as.numeric(day) * 86400 + sum(clock * c(3600, 60, 1)) - offset
}

.seconds <- function(value) {
  seconds <- if (inherits(value, "POSIXt")) {
    as.numeric(as.POSIXct(value))
  } else if (inherits(value, "Date")) {
    as.numeric(value) * 86400
  } else if (is.character(value)) {
    vapply(value, .iso_seconds, numeric(1), USE.NAMES = FALSE)
  } else {
    stop("a time coordinate is a Date, a POSIXct or an ISO string", call. = FALSE)
  }
  if (any(!is.finite(seconds)) || any(abs(seconds) > 2^53))
    stop("`time` must contain finite seconds within R's exact integer range",
         call. = FALSE)
  if (any(seconds != floor(seconds)))
    stop("a time coordinate carries a fraction of a second; rumi records whole seconds",
         call. = FALSE)
  seconds
}

# One instant per step, or a start and an end per step, as POSIX seconds.
.time_axis <- function(time) {
  if (is.null(time))
    stop("every file labels its time steps; pass one date per step, or a start and an end per step",
         call. = FALSE)
  if (is.list(time)) {
    if (any(lengths(time) != 2L))
      stop("an interval step is a start and an end", call. = FALSE)
    kind <- 1L
    seconds <- unlist(lapply(time, .seconds))
  } else if (!is.null(dim(time))) {
    if (length(dim(time)) != 2L || ncol(time) != 2L)
      stop("an interval axis has one row per step, with a start and an end column",
           call. = FALSE)
    kind <- 1L
    seconds <- unlist(lapply(seq_len(nrow(time)), function(i) .seconds(time[i, ])))
  } else {
    kind <- 2L
    seconds <- .seconds(time)
  }
  list(kind = kind, seconds = as.double(seconds))
}

.epsg <- function(crs) {
  if (is.numeric(crs) && length(crs) == 1L && !is.na(crs) &&
      crs == floor(crs) && crs >= 1 && crs <= .Machine$integer.max)
    return(as.integer(crs))
  if (is.character(crs) && length(crs) == 1L && !is.na(crs)) {
    text <- sub("^EPSG:", "", trimws(crs), ignore.case = TRUE)
    if (grepl("^[0-9]+$", text)) {
      code <- suppressWarnings(as.integer(text))
      if (!is.na(code) && code > 0L)
        return(code)
    }
  }
  stop("`crs` must be an EPSG code, such as 32631 or \"EPSG:32631\"", call. = FALSE)
}

.transform <- function(value) {
  if (!is.numeric(value) || !length(value) %in% c(6L, 9L))
    stop("`transform` must be six coefficients (x_res, row_rot, x_origin, col_rot, y_res, y_origin)",
         call. = FALSE)
  if (length(value) == 9L && !identical(as.double(value[7:9]), c(0, 0, 1)))
    stop("a nine-value affine `transform` must end in (0, 0, 1)", call. = FALSE)
  as.double(value[1:6])
}

#' Write compressed frames to a Rumi file
#'
#' Stores the payloads as given and returns the external header. A failed
#' write removes the incomplete file.
#'
#' @param path A local file path. An existing file is replaced.
#' @param frames A `rumi_frames` list with a raw payload in every
#'   `compressed` entry.
#' @param bands One non-empty, unique text per band, in band order, such as
#'   `"B4, Red, 665nm"`.
#' @param time One entry per time step. An instant is a `Date`, a `POSIXct` or
#'   an ISO string such as `"2025-08-08"`. For steps that cover an interval,
#'   pass a two-column matrix or a list of start and end pairs.
#' @param transform Six affine coefficients in rasterio order,
#'   `(x_res, row_rot, x_origin, col_rot, y_res, y_origin)`, given with `crs`.
#' @param crs An EPSG code, such as `32631` or `"EPSG:32631"`.
#' @param pixel_is_point Whether coordinates refer to pixel centres rather
#'   than corners.
#' @return The external header as a raw vector. Keep it with the path.
#' @export
rumi_write <- function(path, frames, bands, time, transform = NULL, crs = NULL,
                       pixel_is_point = FALSE) {
  if (!is.character(path) || length(path) != 1L || is.na(path) || !nzchar(path))
    stop("`path` must be one non-empty string", call. = FALSE)
  if (!inherits(frames, "rumi_frames"))
    stop("`frames` must come from rumi_frames()", call. = FALSE)
  if (length(frames$compressed) != length(frames$data))
    stop("`compressed` must have one entry per frame", call. = FALSE)
  missing <- which(!vapply(frames$compressed, is.raw, logical(1)))[1L]
  if (!is.na(missing))
    stop(sprintf("frame %d has no compressed bytes", missing), call. = FALSE)
  if (!is.character(bands) || anyNA(bands))
    stop("`bands` must be one text per band", call. = FALSE)
  if (!is.logical(pixel_is_point) || length(pixel_is_point) != 1L ||
      is.na(pixel_is_point))
    stop("`pixel_is_point` must be TRUE or FALSE", call. = FALSE)
  axis <- .time_axis(time)
  shape <- c(frames$image_width, frames$image_length, frames$time_count,
             frames$tile_size, frames$bands, frames$frame_unit)
  .rumi_call(rumi_r_write, enc2utf8(path.expand(path)), frames$compressed,
             as.double(shape), frames$datatype,
             if (!is.null(transform)) .transform(transform),
             if (!is.null(crs)) .epsg(crs), pixel_is_point, enc2utf8(bands),
             axis$kind, as.double(axis$seconds))
}

#' @export
print.rumi_frames <- function(x, ...) {
  done <- sum(vapply(x$compressed, is.raw, logical(1)))
  cat("<Rumi frames> ", length(x$data), " frames of ", x$layout, ", ",
      x$image_width, " x ", x$image_length, " in tiles of ", x$tile_size, ", ",
      x$datatype, ", ", done, " compressed\n", sep = "")
  invisible(x)
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
