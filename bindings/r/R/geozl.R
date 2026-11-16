# The argument checks below mirror the geozl R package, word for word, with
# rumi_geozl_ in place of geozl_ where a message names a call. The core checks
# what it can see, such as the error text and the plane split.

# Accept terra and GDAL names for the same storage types.
.aliases <- c(
  INT1U = "uint8", INT2U = "uint16", INT4U = "uint32", INT8U = "uint64",
  INT1S = "int8", INT2S = "int16", INT4S = "int32", INT8S = "int64",
  FLT4S = "float32", FLT8S = "float64",
  Byte = "uint8", UInt16 = "uint16", UInt32 = "uint32", UInt64 = "uint64",
  Int8 = "int8", Int16 = "int16", Int32 = "int32", Int64 = "int64",
  Float16 = "float16", Float32 = "float32", Float64 = "float64"
)

.canonical <- function(name) {
  if (name %in% names(.aliases)) .aliases[[name]] else name
}

# The storage type of a raster, given or implied by R's own type. A frame from
# rumi_frames() names the type it is written as, so a graph takes that type and
# refuses another.
.geozl_dtype <- function(dtype, x = NULL) {
  framed <- attr(x, "rumi_dtype", exact = TRUE)
  given <- if (is.null(dtype)) framed else dtype
  if (is.null(given)) {
    if (is.null(x))
      stop("`dtype` is required", call. = FALSE)
    given <- switch(typeof(x),
      raw = "uint8", logical = "bool", integer = "int32", double = "float64",
      stop(sprintf("`x` must be a double, integer, logical or raw vector, not %s",
                   typeof(x)), call. = FALSE))
  }
  if (!is.character(given) || length(given) != 1L || is.na(given))
    stop("`dtype` must be a single string", call. = FALSE)
  name <- .canonical(given)
  sizes <- .rumi_call(rumi_r_dtype_sizes)
  if (!name %in% names(sizes))
    stop(sprintf(paste0("unknown dtype \"%s\"; use one of %s, or terra's ",
                        "(\"INT2U\") or GDAL's (\"UInt16\") names"),
                 given, paste(names(sizes), collapse = ", ")),
         call. = FALSE)
  if (!is.null(framed) && !identical(name, .canonical(framed)))
    stop("`dtype` is not the one rumi_frames() gave this frame", call. = FALSE)
  list(name = name, itemsize = sizes[[name]],
       integer = grepl("^u?int[0-9]+$", name))
}

# Format a numeric bound for a LINEAR recipe. Integral doubles use integer text;
# other values use a decimal that reads back as the same double.
.format_number <- function(x) {
  if (!is.finite(x))
    return(format(x))
  if (x == floor(x) && abs(x) < 1e15)
    return(format(x, scientific = FALSE))
  for (digits in 15:17) {
    text <- sprintf("%.*g", digits, x)
    if (as.numeric(text) == x)
      break
  }
  text
}

# The error as text for the core, which expands and checks it. R checks only
# what the text cannot carry.
.error_text <- function(error) {
  if (is.null(error))
    return(NULL)
  if (is.logical(error))
    stop("`error` must not be a logical", call. = FALSE)
  if (length(error) != 1L || is.na(error))
    stop("`error` must be a single number, percentage or recipe", call. = FALSE)
  if (is.numeric(error))
    return(.format_number(error))
  if (!is.character(error))
    stop("`error` must be a number, percentage or recipe", call. = FALSE)
  # A bare number is LINEAR only as a number. The core gets text either way, so
  # the binding tells them apart, as in the geozl packages.
  if (!grepl(":", error, fixed = TRUE) && !endsWith(error, "%"))
    stop(sprintf(paste0("`error` must be a number, a percentage such as \"10%%\", ",
                        "or a LINEAR, LOG or SQRT recipe, got \"%s\""), error),
         call. = FALSE)
  enc2utf8(error)
}

.whole <- function(value, name) {
  if (!is.numeric(value) || length(value) != 1L || is.na(value) ||
      !is.finite(value) || value < 1 || value != floor(value))
    stop(sprintf("`%s` must be a whole number of at least 1", name), call. = FALSE)
  as.double(value)
}

# R's first dimension is the row, unlike Python's last dimension.
.geometry <- function(x, n, width, planes) {
  d <- dim(x)
  if (!is.null(d) && prod(d) != n)
    d <- NULL
  if (is.null(width)) {
    if (length(d) < 2L)
      stop("give `width` for a vector without dim", call. = FALSE)
    width <- d[[1L]]
  }
  if (is.null(planes))
    planes <- if (length(d) >= 3L) d[[length(d)]] else 1L
  list(width = .whole(width, "width"), planes = .whole(planes, "planes"))
}

.nodata <- function(nodata, dt) {
  if (is.null(nodata))
    return(NULL)
  if (length(nodata) != 1L || !is.numeric(nodata) && !identical(nodata, NA))
    stop("`nodata` must be a single number or NA", call. = FALSE)
  if (is.na(nodata) && dt$integer)
    stop(sprintf("a %s raster cannot use NaN as nodata", dt$name), call. = FALSE)
  as.double(nodata)
}

.pack <- function(x, dt, nodata) {
  value <- if (!is.null(nodata) && !is.na(nodata)) as.double(nodata)
  .rumi_call(rumi_r_geozl_pack, x, dt$name, value)
}

#' Build a reusable compression graph
#'
#' Build a graph with the GeoZL linked into Rumi. It fixes the recipe, storage
#' type and predictor geometry for later tiles.
#'
#' `width` defaults to `dim(x)[1]`; `planes` defaults to the last dimension
#' for arrays with at least three dimensions. Row predictors restart at each
#' plane.
#'
#' Samples are converted to `dtype` before compression. Integer values must
#' fit exactly; a raw vector already contains native samples.
#'
#' A bad argument stops with a plain error whose call is `NULL`. R checks what
#' the core cannot see in the words the geozl R package uses, and the core
#' checks the error text, the plane split and the dtype. A refusal by the
#' native library follows Rumi's rule. An invalid request, such as an unknown
#' recipe, is a plain error, and any other refusal signals a
#' `rumi_error_<kind>` condition.
#'
#' Rumi covers building a graph and compressing a tile. Decompression,
#' profiling and frame coefficients live in the geozl package.
#'
#' @param x Raster samples as a double, integer, logical or raw vector or array.
#'   For lossy compression, use data covering the intended range of later tiles.
#' @param method A recipe such as `"planar>zigzag>pfor"`.
#' @param width,planes Row width in samples and number of stacked images.
#' @param error A number (LINEAR), percentage (LOG), or full recipe. `NULL`
#'   and zero mean lossless.
#' @param nodata A sentinel for missing samples. Pass `NaN` to mark all NaNs;
#'   float rasters with NaNs also detect them when `nodata` is `NULL`.
#' @param dtype Storage type, as [rumi_frames()] takes it. terra's
#'   `"INT2U"` and GDAL's `"UInt16"` names are accepted too. A frame from
#'   [rumi_frames()] uses its own dtype and refuses another. Otherwise it
#'   defaults to `"int32"` for integer, `"float64"` for double, `"uint8"` for
#'   raw and `"bool"` for logical input.
#' @return A `rumi_geozl_graph` with the fields `method`, `width`, `planes`,
#'   `dtype`, `itemsize`, `error` and `nodata`. Its native context is not safe
#'   for concurrent calls and does not survive a new R session.
#' @examples
#' tile <- matrix(1000L + 1:(64 * 64) %% 97L, 64, 64)
#' g <- rumi_geozl_graph(tile, "planar>zigzag>pfor", dtype = "uint16")
#' frame <- rumi_geozl_compress(tile, g)
#' @export
rumi_geozl_graph <- function(x, method, width = NULL, planes = NULL,
                             error = NULL, nodata = NULL, dtype = NULL) {
  if (!is.character(method) || length(method) != 1L || is.na(method) ||
      !nzchar(method))
    stop("`method` must be a recipe such as \"planar>zigzag>pfor\"",
         call. = FALSE)
  recipe <- .error_text(error)
  dt <- .geozl_dtype(dtype, x)
  value <- .nodata(nodata, dt)
  bytes <- .pack(x, dt, value)
  geometry <- .geometry(x, length(bytes) %/% dt$itemsize, width, planes)
  ptr <- .rumi_call(rumi_r_geozl_graph, bytes, enc2utf8(method), geometry$width,
                    geometry$planes, recipe, dt$name, value)
  structure(list(ptr = ptr, method = method, width = geometry$width,
                 planes = geometry$planes, dtype = dt$name,
                 itemsize = as.double(dt$itemsize),
                 error = .rumi_call(rumi_r_geozl_error, ptr), nodata = nodata),
            class = "rumi_geozl_graph")
}

#' @export
print.rumi_geozl_graph <- function(x, ...) {
  plane_word <- if (x$planes == 1) "plane" else "planes"
  parts <- c(x$method, x$dtype, paste("row width", format(x$width)),
             paste(format(x$planes), plane_word),
             if (is.null(x$error)) "lossless" else x$error)
  cat("<rumi_geozl_graph> ", paste(parts, collapse = ", "), "\n", sep = "")
  invisible(x)
}

#' Compress a tile
#'
#' @param x Tile samples, converted to the graph's dtype. Its geometry is
#'   not checked against the graph.
#' @param graph A graph from [rumi_geozl_graph()].
#' @return The frame, as a raw vector, ready for a `compressed` entry of
#'   [rumi_frames()].
#' @export
rumi_geozl_compress <- function(x, graph) {
  if (!inherits(graph, "rumi_geozl_graph"))
    stop("`graph` must come from rumi_geozl_graph()", call. = FALSE)
  framed <- attr(x, "rumi_dtype", exact = TRUE)
  if (!is.null(framed) && !identical(.canonical(framed), graph$dtype))
    stop("this frame's dtype is not the graph's", call. = FALSE)
  dt <- .geozl_dtype(graph$dtype)
  .rumi_call(rumi_r_geozl_compress, graph$ptr, .pack(x, dt, graph$nodata),
             dt$name)
}
