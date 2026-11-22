.rumi_call <- function(name, ...) {
  .Call(name, ..., PACKAGE = "rumi")
}

.header <- function(header) {
  if (missing(header) || !is.raw(header))
    stop("`header` must be a raw vector", call. = FALSE)
  header
}

# Whole UTC days read back as Date, as Python returns date.
.time_values <- function(seconds) {
  if (all(seconds %% 86400 == 0))
    return(as.Date(seconds / 86400))
  as.POSIXct(seconds, origin = "1970-01-01", tz = "UTC")
}

.time_text <- function(time) {
  if (inherits(time, "Date"))
    return(format(time))
  format(time, tz = "UTC", usetz = TRUE)
}

.ordinal <- function(n) {
  suffix <- if (n %% 100 %in% 11:13) "th" else
    switch(as.character(n %% 10), "1" = "st", "2" = "nd", "3" = "rd", "th")
  paste0(n, suffix)
}

# Seconds from 0001-01-01 to 9999-12-31T23:59:59 UTC, the years the writer
# and the other bindings accept.
.time_limits <- c(-62135596800, 253402300799)

.metadata <- function(value) {
  if (!is.null(value$time)) {
    outside <- which(value$time < .time_limits[[1L]] |
                     value$time > .time_limits[[2L]])
    if (length(outside)) {
      step <- outside[[1L]]
      if (identical(value$time_kind, "interval"))
        step <- (step + 1L) %/% 2L
      .rumi_error("format", sprintf(
        "the %s time step falls outside the years 1 to 9999", .ordinal(step)))
    }
    time <- .time_values(value$time)
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

# The C glue signals every native status other than an invalid request here.
.rumi_error <- function(kind, message) {
  stop(structure(
    class = c(paste0("rumi_error_", kind), "rumi_error", "error", "condition"),
    list(message = message, call = NULL, kind = kind)
  ))
}

.source <- function(source) {
  if (is.raw(source))
    return(source)
  if (!is.character(source) || length(source) != 1L || is.na(source) ||
      !nzchar(source))
    stop("`source` must be one non-empty path or a raw vector of file bytes",
         call. = FALSE)
  enc2utf8(path.expand(source))
}

# A read takes the header alone, because checking it against the source
# reads the index and trailer and costs range requests on a remote source.
.pair <- function(source, header) {
  value <- .rumi_call(rumi_r_open, .source(source), .header(header))
  value$metadata <- .metadata(value$metadata)
  value
}

.release <- function(pair) invisible(.rumi_call(rumi_r_close, pair$pointer))

#' Inspect a Rumi source or header
#'
#' A source alone is read and its canonical external header rebuilt. A header
#' alone reads no source, so `bands`, `time`, `time_kind`, `transform`, `crs`
#' and `pixel_is_point` are `NULL`. Given both, Rumi checks that they match.
#'
#' @param source A local path, URI or VSI path, or the file's bytes as a raw
#'   vector.
#' @param header Raw external-header bytes.
#' @return A `rumi_metadata` list. Its `time` holds `Date` values when every
#'   coordinate is a whole UTC day and UTC `POSIXct` values otherwise.
#' @export
rumi_info <- function(source = NULL, header = NULL) {
  if (!is.null(source))
    source <- .source(source)
  if (!is.null(header))
    header <- .header(header)
  .metadata(.rumi_call(rumi_r_info, source, header))
}

.selection <- function(value, name) {
  if (is.null(value))
    return(NULL)
  if (!is.null(dim(value)))
    stop(sprintf("`%s` must be a vector of indices, not a matrix or array", name),
         call. = FALSE)
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

# The header names no bands or time steps, so only the axes are named.
.decorate <- function(x, metadata, batch = FALSE) {
  axes <- c("x", "y", "band")
  if (metadata$time_count > 1L)
    axes <- c(axes, "time")
  if (batch)
    axes <- c(axes, "n")
  attr(x, "rumi_axes") <- axes
  attr(x, "rumi_metadata") <- metadata
  labels <- vector("list", length(dim(x)))
  names(labels) <- axes
  dimnames(x) <- labels
  x
}

#' Read a partial Rumi array
#'
#' `bands`, `time` and window origins use R's one-based indexing. `window` is
#' `(row, column, height, width)`. The returned Image is `(X, Y, Band)` and a
#' Cube is `(X, Y, Band, Time)`, matching R's column-major memory order.
#'
#' A read uses the header alone, without the index and trailer, so the result
#' has no band or time dimnames and its `rumi_metadata` attribute holds only
#' the header's fields. [rumi_info()] with both `source` and `header` checks
#' the pair and returns the band texts and time coordinates.
#'
#' Native failures other than an invalid request signal a condition of class
#' `rumi_error_<kind>` and `rumi_error`, with the kind, such as `"io"` or
#' `"decode"`, in its `kind` field.
#'
#' @param source A local path, URI or VSI path, or the file's bytes as a raw
#'   vector.
#' @param header Raw external-header bytes for `source`.
#' @param time,bands One-based indices, in output order, or `NULL` for all.
#' @param window One-based `(row, column, height, width)`, or `NULL` for all.
#' @param pattern Optional Rumi output pattern.
#' @export
rumi_read <- function(source, header, time = NULL, bands = NULL,
                      window = NULL, pattern = NULL) {
  pair <- .pair(source, header)
  on.exit(.release(pair), add = TRUE)
  time <- .selection(time, "time")
  bands <- .selection(bands, "bands")
  x <- .rumi_call(rumi_r_read, pair$pointer, time, bands,
                  .window(window), .pattern(pattern))
  if (is.null(pattern))
    x <- .decorate(x, pair$metadata)
  x
}

#' Read one window from several Rumi sources
#'
#' Each source is read with its header alone, as in [rumi_read()], so the
#' result has no band or time dimnames.
#'
#' @param sources A list or character vector of paths and raw vectors of file
#'   bytes.
#' @param headers A list with one raw external header per source.
#' @param windows A list or matrix with one `(row, column, height, width)` row
#'   per source. All windows have the same height and width. A `NULL` entry,
#'   or `NULL` for all, reads the whole image.
#' @inheritParams rumi_read
#' @return An array with a last `n` dimension, one entry per source.
#' @export
rumi_read_many <- function(sources, headers, windows = NULL, time = NULL,
                           bands = NULL, pattern = NULL) {
  if (is.raw(sources))
    stop("`sources` must be a list with one entry per item; use list(source)",
         call. = FALSE)
  if (is.character(sources))
    sources <- as.list(sources)
  if (!is.list(sources) || !length(sources))
    stop("`sources` must be a non-empty list of sources", call. = FALSE)
  if (missing(headers) || !is.list(headers) ||
      length(headers) != length(sources))
    stop("`headers` must be a list with one raw header per source",
         call. = FALSE)
  pairs <- vector("list", length(sources))
  on.exit(lapply(Filter(Negate(is.null), pairs), .release), add = TRUE)
  for (i in seq_along(sources))
    pairs[[i]] <- .pair(sources[[i]], headers[[i]])
  if (is.null(windows)) {
    windows <- vector("list", length(pairs))
  } else if (is.matrix(windows)) {
    windows <- lapply(seq_len(nrow(windows)), function(i) windows[i, ])
  }
  if (!is.list(windows) || length(windows) != length(pairs))
    stop("`windows` must have one window per source", call. = FALSE)
  windows <- lapply(windows, .window)
  time <- .selection(time, "time")
  bands <- .selection(bands, "bands")
  x <- .rumi_call(rumi_r_read_many, lapply(pairs, `[[`, "pointer"), windows,
                  time, bands, .pattern(pattern))
  if (is.null(pattern))
    x <- .decorate(x, pairs[[1L]]$metadata, batch = TRUE)
  x
}

.dtype <- function(value, x) {
  if (is.null(value))
    return(switch(typeof(x), integer = "int32", double = "float64",
                  raw = "uint8", logical = "bool", complex = "complex128",
                  stop("`x` must be a raw, logical, integer, double or complex array",
                       call. = FALSE)))
  if (!is.character(value) || length(value) != 1L || is.na(value) || !nzchar(value))
    stop("`dtype` must be one sample type such as \"uint16\"", call. = FALSE)
  value
}

.frame_pattern <- function(pattern) {
  if (!is.character(pattern) || length(pattern) != 1L || is.na(pattern) ||
      !nzchar(pattern))
    stop("`pattern` must be one frame pattern", call. = FALSE)
  enc2utf8(pattern)
}

.tile_size <- function(tile_size) {
  if (!is.numeric(tile_size) || length(tile_size) != 1L || is.na(tile_size) ||
      tile_size != floor(tile_size))
    stop("`tile_size` must be a whole number", call. = FALSE)
  if (tile_size < 0)
    stop("`tile_size` must not be negative", call. = FALSE)
  if (tile_size > 65535)
    stop("`tile_size` must be at most 65535", call. = FALSE)
  as.integer(tile_size)
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
#' @param dtype The sample type the payloads are compressed as, such as
#'   `"uint16"`. Defaults to `"int32"` for integer arrays, `"float64"` for
#'   double, `"uint8"` for raw, `"bool"` for logical and `"complex128"` for
#'   complex.
#' @return A `rumi_frames` list with `data`, one array per frame in file
#'   order, `compressed`, a list of `NULL` to fill, and the layout of the file.
#'   `tiles_across` and `tiles_down` give the grid. `row`, `col`, `band` and
#'   `time` give each frame's one-based position. `band` and `time` are `NULL`
#'   unless frames are split by that axis. Each frame carries `dtype` as its
#'   `rumi_dtype` attribute, which [rumi_geozl_graph()] uses.
#' @export
rumi_frames <- function(x, pattern, tile_size = 512L, dtype = NULL) {
  if (is.null(dim(x)))
    stop("`x` must be an array", call. = FALSE)
  pattern <- .frame_pattern(pattern)
  tile_size <- .tile_size(tile_size)
  dtype <- .dtype(dtype, x)
  value <- .rumi_call(rumi_r_frames, x, pattern, tile_size, dtype)
  value <- c(value[1L], list(compressed = vector("list", length(value$data))),
             value[-1L])
  value$dtype <- dtype
  value$pattern <- trimws(pattern)
  class(value) <- "rumi_frames"
  value
}

#' Find where the first frame starts
#'
#' Returns the byte offset of the first frame without cutting an array, which
#' is also the size of everything a file stores before its first frame.
#'
#' @param shape The dimensions of the array [rumi_frames()] would cut, in
#'   `dim()` order, the reverse of the pattern's input axes.
#' @inheritParams rumi_frames
#' @return The offset in bytes, as a double.
#' @export
rumi_frame_start <- function(shape, pattern, tile_size = 512L) {
  if (!is.numeric(shape) || !length(shape) || anyNA(shape) ||
      any(!is.finite(shape)) || any(shape != floor(shape)) || any(shape < 0))
    stop("`shape` must hold whole, non-negative dimensions", call. = FALSE)
  .rumi_call(rumi_r_frame_start, as.double(shape), .frame_pattern(pattern),
             .tile_size(tile_size))
}

#' Thread and checksum settings
#'
#' The thread count and OpenZL checksum verification are process-wide. Set
#' them before the first read. The first parallel read pins the thread count
#' and the first decoded frame pins checksum verification, after which a
#' change signals a `rumi_error_state` condition. `RUMI_NUM_THREADS` and
#' `RUMI_VERIFY` give the initial values.
#'
#' @param n A whole number of threads from 1 to 1024.
#' @param on `TRUE` to verify OpenZL checksums.
#' @return The setters return the accepted value invisibly. The getters
#'   return the current value.
#' @name rumi_settings
NULL

#' @rdname rumi_settings
#' @export
rumi_set_num_threads <- function(n) {
  if (!is.numeric(n) || length(n) != 1L || is.na(n) || n != floor(n) ||
      abs(n) > .Machine$integer.max)
    stop("`n` must be one whole number", call. = FALSE)
  n <- as.integer(n)
  .rumi_call(rumi_r_set_num_threads, n)
  invisible(n)
}

#' @rdname rumi_settings
#' @export
rumi_get_num_threads <- function() .rumi_call(rumi_r_get_num_threads)

#' @rdname rumi_settings
#' @export
rumi_set_checksum_verification <- function(on) {
  if (!is.logical(on) || length(on) != 1L || is.na(on))
    stop("`on` must be TRUE or FALSE", call. = FALSE)
  .rumi_call(rumi_r_set_checksum_verification, on)
  invisible(on)
}

#' @rdname rumi_settings
#' @export
rumi_get_checksum_verification <- function() {
  .rumi_call(rumi_r_get_checksum_verification)
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

# EPSG codes span 1 to 2^32 - 1, past R's integer range, so they travel as
# doubles.
.epsg <- function(crs) {
  if (inherits(crs, "crs"))
    crs <- crs$epsg
  if (is.character(crs) && length(crs) == 1L && !is.na(crs)) {
    text <- sub("^EPSG:", "", trimws(crs), ignore.case = TRUE)
    crs <- if (grepl("^[0-9]+$", text)) as.numeric(text)
  }
  if (is.numeric(crs) && length(crs) == 1L && !is.na(crs) &&
      crs == floor(crs) && crs >= 1 && crs <= 4294967295)
    return(as.double(crs))
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
#' @param crs An EPSG code from 1 to 4294967295, such as `32631` or
#'   `"EPSG:32631"`, or an sf `crs` object with an EPSG code.
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
             as.double(shape), frames$dtype,
             if (!is.null(transform)) .transform(transform),
             if (!is.null(crs)) .epsg(crs), pixel_is_point, enc2utf8(bands),
             axis$kind, as.double(axis$seconds))
}

#' @export
print.rumi_frames <- function(x, ...) {
  done <- sum(vapply(x$compressed, is.raw, logical(1)))
  cat("<Rumi frames> ", length(x$data), " frames of ", x$layout, ", ",
      x$image_width, " x ", x$image_length, " in tiles of ", x$tile_size, ", ",
      x$dtype, ", ", done, " compressed\n", sep = "")
  invisible(x)
}

#' @export
print.rumi_metadata <- function(x, ...) {
  cat("  shape:", paste(x$shape, collapse = " x "), "\n")
  cat("  dtype:", x$dtype, "\n")
  cat("  tile:", paste(x$tile, collapse = " x "), "\n")
  cat("  frame layout:", x$frame_layout, "\n")
  if (!is.null(x$bands))
    cat("  bands:", paste(x$bands, collapse = ", "), "\n")
  if (!is.null(x$crs))
    cat("  CRS: EPSG:", format(x$crs, scientific = FALSE), "\n", sep = "")
  if (length(x$time)) {
    first <- if (is.matrix(x$time)) {
      paste(.time_text(x$time[1L, 1L]), .time_text(x$time[1L, 2L]), sep = "/")
    } else {
      .time_text(x$time[[1L]])
    }
    cat("  time:", first, "...\n")
  }
  invisible(x)
}
