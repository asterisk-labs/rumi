#define R_NO_REMAP
#include <R.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>
#include <Rinternals.h>

#include "rumi/rumi.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rumi_r_abort(void) { Rf_error("OpenZL failed an internal check"); }

typedef struct {
  rumi_source *source;
  rumi_spec *spec;
} dataset;

static void fail(rumi_status status) {
  if (status == RUMI_OK)
    return;
  const char *message = rumi_last_error();
  Rf_error("%s", message == NULL ? "librumi failed" : message);
}

static dataset *dataset_of(SEXP pointer) {
  if (TYPEOF(pointer) != EXTPTRSXP ||
      R_ExternalPtrTag(pointer) != Rf_install("rumi_dataset"))
    Rf_error("invalid Rumi dataset pointer");
  dataset *value = R_ExternalPtrAddr(pointer);
  if (value == NULL || value->source == NULL || value->spec == NULL)
    Rf_error("this Rumi dataset is closed");
  return value;
}

static void dataset_close(dataset *value) {
  if (value == NULL)
    return;
  if (value->source != NULL)
    rumi_source_free(value->source);
  if (value->spec != NULL)
    rumi_spec_destroy(value->spec);
  free(value);
}

static void dataset_finalizer(SEXP pointer) {
  dataset_close(R_ExternalPtrAddr(pointer));
  R_ClearExternalPtr(pointer);
}

static SEXP names(SEXP value, const char *const *items, int n) {
  SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
  for (int i = 0; i < n; ++i)
    SET_STRING_ELT(out, i, Rf_mkChar(items[i]));
  Rf_setAttrib(value, R_NamesSymbol, out);
  UNPROTECT(1);
  return value;
}

static const rumi_dtype_info *dtype_info(rumi_dtype dtype) {
  const rumi_dtype_info *registry = NULL;
  size_t count = rumi_dtype_registry(&registry);
  for (size_t i = 0; i < count; ++i)
    if (registry[i].code == (uint8_t)dtype)
      return registry + i;
  Rf_error("librumi returned unknown dtype code %d", (int)dtype);
}

static SEXP metadata_to_r(rumi_metadata *metadata) {
  static const char *const field_names[] = {
      "header", "shape", "time_count", "dtype", "tile", "frame_layout",
      "index_order", "frames", "bands", "time", "time_kind", "transform",
      "crs", "pixel_is_point"};
  rumi_header *h = &metadata->fields;
  SEXP out = PROTECT(Rf_allocVector(VECSXP, 14));

  SEXP header = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t)metadata->blob_size));
  memcpy(RAW(header), metadata->blob, metadata->blob_size);
  SET_VECTOR_ELT(out, 0, header);

  int ndim = h->time_count > 1 ? 4 : 3;
  SEXP shape = PROTECT(Rf_allocVector(REALSXP, ndim));
  int at = 0;
  if (ndim == 4)
    REAL(shape)[at++] = h->time_count;
  REAL(shape)[at++] = h->samples_per_pixel;
  REAL(shape)[at++] = h->image_length;
  REAL(shape)[at] = h->image_width;
  SET_VECTOR_ELT(out, 1, shape);
  SET_VECTOR_ELT(out, 2, Rf_ScalarReal(h->time_count));
  SET_VECTOR_ELT(out, 3, Rf_mkString(dtype_info(h->dtype)->name));

  SEXP tile = PROTECT(Rf_allocVector(INTSXP, 2));
  INTEGER(tile)[0] = h->tile_length;
  INTEGER(tile)[1] = h->tile_width;
  SET_VECTOR_ELT(out, 4, tile);

  char layout[16] = {0};
  fail(rumi_unit_name(h->frame_unit, h->samples_per_pixel, h->time_count,
                      layout, sizeof layout));
  SET_VECTOR_ELT(out, 5, Rf_mkString(layout));

  uint8_t axes[2] = {0};
  int naxes = 0;
  fail(rumi_unit_index_axes(h->frame_unit, h->samples_per_pixel,
                            h->time_count, axes, &naxes));
  SEXP order = PROTECT(Rf_allocVector(STRSXP, naxes));
  uint64_t frames = (uint64_t)h->tiles_across * h->tiles_down;
  for (int i = 0; i < naxes; ++i) {
    const char *axis = rumi_axis_name(axes[i]);
    SET_STRING_ELT(order, i, Rf_mkChar(axis));
    if (strcmp(axis, "b") == 0)
      frames *= h->samples_per_pixel;
    if (strcmp(axis, "t") == 0)
      frames *= h->time_count;
  }
  SET_VECTOR_ELT(out, 6, order);
  SET_VECTOR_ELT(out, 7, Rf_ScalarReal((double)frames));

  SEXP bands = PROTECT(Rf_allocVector(STRSXP, metadata->band_text_count));
  for (size_t i = 0; i < metadata->band_text_count; ++i)
    SET_STRING_ELT(bands, i, Rf_mkCharCE(metadata->band_texts[i], CE_UTF8));
  SET_VECTOR_ELT(out, 8, bands);

  SEXP time = PROTECT(Rf_allocVector(REALSXP, metadata->time_coords));
  for (size_t i = 0; i < metadata->time_coords; ++i)
    REAL(time)[i] = (double)metadata->time[i];
  SET_VECTOR_ELT(out, 9, time);
  const char *kind = metadata->time_type == 1 ? "interval" :
                     metadata->time_type == 2 ? "instant" : NULL;
  SET_VECTOR_ELT(out, 10, kind == NULL ? R_NilValue : Rf_mkString(kind));

  if (metadata->epsg != 0) {
    SEXP transform = PROTECT(Rf_allocVector(REALSXP, 6));
    memcpy(REAL(transform), metadata->transform, 6 * sizeof(double));
    SET_VECTOR_ELT(out, 11, transform);
    SET_VECTOR_ELT(out, 12, Rf_ScalarReal(metadata->epsg));
    UNPROTECT(1);
  }
  SET_VECTOR_ELT(out, 13, Rf_ScalarLogical(metadata->pixel_is_point != 0));
  names(out, field_names, 14);
  UNPROTECT(7);
  return out;
}

static SEXP metadata_to_r_unwind(void *data) {
  return metadata_to_r(data);
}

static void metadata_free_unwind(void *data, Rboolean jump) {
  (void)jump;
  rumi_metadata_free(data);
}

SEXP rumi_r_open(SEXP path_s, SEXP header_s) {
  if (TYPEOF(path_s) != STRSXP || XLENGTH(path_s) != 1 ||
      STRING_ELT(path_s, 0) == NA_STRING)
    Rf_error("`path` must be one string");
  if (TYPEOF(header_s) != RAWSXP)
    Rf_error("`header` must be raw");

  dataset *value = calloc(1, sizeof(*value));
  if (value == NULL)
    Rf_error("could not allocate Rumi dataset");
  rumi_status status = rumi_spec_parse(RAW(header_s), XLENGTH(header_s),
                                       &value->spec);
  if (status == RUMI_OK)
    status = rumi_source_file(Rf_translateCharUTF8(STRING_ELT(path_s, 0)),
                              &value->source);
  if (status != RUMI_OK) {
    const char *message = rumi_last_error();
    char copy[512];
    snprintf(copy, sizeof copy, "%s",
             message == NULL ? "librumi failed" : message);
    dataset_close(value);
    Rf_error("%s", copy);
  }

  rumi_metadata native = {0};
  status = rumi_info(value->source, RAW(header_s), XLENGTH(header_s), &native);
  if (status != RUMI_OK) {
    const char *message = rumi_last_error();
    char copy[512];
    snprintf(copy, sizeof copy, "%s",
             message == NULL ? "librumi failed" : message);
    dataset_close(value);
    Rf_error("%s", copy);
  }

  SEXP pointer = PROTECT(R_MakeExternalPtr(
      value, Rf_install("rumi_dataset"), R_NilValue));
  R_RegisterCFinalizerEx(pointer, dataset_finalizer, TRUE);
  SEXP metadata = PROTECT(R_UnwindProtect(
      metadata_to_r_unwind, &native, metadata_free_unwind, &native, NULL));
  SEXP out = PROTECT(Rf_allocVector(VECSXP, 4));
  SET_VECTOR_ELT(out, 0, pointer);
  SET_VECTOR_ELT(out, 1, path_s);
  SET_VECTOR_ELT(out, 2, header_s);
  SET_VECTOR_ELT(out, 3, metadata);
  static const char *const out_names[] = {"pointer", "path", "header", "metadata"};
  names(out, out_names, 4);
  UNPROTECT(3);
  return out;
}

SEXP rumi_r_close(SEXP pointer) {
  if (TYPEOF(pointer) != EXTPTRSXP ||
      R_ExternalPtrTag(pointer) != Rf_install("rumi_dataset"))
    Rf_error("invalid Rumi dataset pointer");
  dataset_close(R_ExternalPtrAddr(pointer));
  R_ClearExternalPtr(pointer);
  return R_NilValue;
}

// NULL selects all indices; an empty vector must stay non-NULL.
static void selection(SEXP value, const char *name, int **out, size_t *count) {
  static int none;
  if (Rf_isNull(value)) {
    *out = NULL;
    *count = 0;
    return;
  }
  if (TYPEOF(value) != INTSXP)
    Rf_error("`%s` must be an integer vector or NULL", name);
  *count = (size_t)XLENGTH(value);
  *out = *count ? INTEGER(value) : &none;
}

static void window(SEXP value, const rumi_header *h,
                   int *y_off, int *y_size, int *x_off, int *x_size) {
  if (Rf_isNull(value)) {
    *y_off = 0;
    *x_off = 0;
    *y_size = (int)h->image_length;
    *x_size = (int)h->image_width;
    return;
  }
  if (TYPEOF(value) != REALSXP || XLENGTH(value) != 4)
    Rf_error("`window` must have four values");
  *y_off = (int)REAL(value)[0] - 1;
  *x_off = (int)REAL(value)[1] - 1;
  *y_size = (int)REAL(value)[2];
  *x_size = (int)REAL(value)[3];
}

static const char *pattern(SEXP value) {
  return Rf_isNull(value) ? NULL : Rf_translateCharUTF8(STRING_ELT(value, 0));
}

static uint16_t half_bits(const uint8_t *src) {
  uint16_t value;
  memcpy(&value, src, sizeof value);
  return value;
}

static double half_value(uint16_t value) {
  int sign = (value & 0x8000u) ? -1 : 1;
  int exponent = (value >> 10) & 31;
  int mantissa = value & 1023;
  if (exponent == 0)
    return sign * ldexp((double)mantissa, -24);
  if (exponent == 31)
    return mantissa ? R_NaN : sign * R_PosInf;
  return sign * ldexp((double)(mantissa + 1024), exponent - 25);
}

static void integer_samples(int *out, const uint8_t *src, rumi_dtype dtype,
                            R_xlen_t samples) {
  switch (dtype) {
  case RUMI_DT_UINT8:
    for (R_xlen_t i = 0; i < samples; ++i)
      out[i] = src[i];
    return;
  case RUMI_DT_INT8:
    for (R_xlen_t i = 0; i < samples; ++i)
      out[i] = ((const int8_t *)src)[i];
    return;
  case RUMI_DT_UINT16:
    for (R_xlen_t i = 0; i < samples; ++i) {
      uint16_t value;
      memcpy(&value, src + 2 * (size_t)i, 2);
      out[i] = value;
    }
    return;
  case RUMI_DT_INT16:
    for (R_xlen_t i = 0; i < samples; ++i) {
      int16_t value;
      memcpy(&value, src + 2 * (size_t)i, 2);
      out[i] = value;
    }
    return;
  default:
    Rf_error("cannot convert dtype %d to an R integer array", (int)dtype);
  }
}

static int real_samples(double *out, const uint8_t *src, rumi_dtype dtype,
                        R_xlen_t samples) {
  int inexact = 0;
  switch (dtype) {
  case RUMI_DT_UINT32:
    for (R_xlen_t i = 0; i < samples; ++i) {
      uint32_t value;
      memcpy(&value, src + 4 * (size_t)i, 4);
      out[i] = value;
    }
    break;
  case RUMI_DT_INT32:
    for (R_xlen_t i = 0; i < samples; ++i) {
      int32_t value;
      memcpy(&value, src + 4 * (size_t)i, 4);
      out[i] = value;
    }
    break;
  case RUMI_DT_UINT64: {
    for (R_xlen_t i = 0; i < samples; ++i) {
      uint64_t value;
      memcpy(&value, src + 8 * (size_t)i, 8);
      inexact |= value > UINT64_C(9007199254740992);
      out[i] = (double)value;
    }
    break;
  }
  case RUMI_DT_INT64: {
    for (R_xlen_t i = 0; i < samples; ++i) {
      int64_t value;
      memcpy(&value, src + 8 * (size_t)i, 8);
      inexact |= value > INT64_C(9007199254740992) ||
                 value < -INT64_C(9007199254740992);
      out[i] = (double)value;
    }
    break;
  }
  case RUMI_DT_FLOAT16:
    for (R_xlen_t i = 0; i < samples; ++i)
      out[i] = half_value(half_bits(src + 2 * (size_t)i));
    break;
  case RUMI_DT_FLOAT32:
    for (R_xlen_t i = 0; i < samples; ++i) {
      float value;
      memcpy(&value, src + 4 * (size_t)i, 4);
      out[i] = value;
    }
    break;
  case RUMI_DT_FLOAT64:
    memcpy(out, src, (size_t)samples * sizeof(*out));
    break;
  case RUMI_DT_BFLOAT16: {
    for (R_xlen_t i = 0; i < samples; ++i) {
      uint32_t bits = (uint32_t)half_bits(src + 2 * (size_t)i) << 16;
      float value;
      memcpy(&value, &bits, 4);
      out[i] = value;
    }
    break;
  }
  default:
    Rf_error("cannot convert dtype %d to an R double array", (int)dtype);
  }
  return inexact;
}

static void complex_samples(Rcomplex *out, const uint8_t *src, int width,
                            R_xlen_t samples) {
  for (R_xlen_t i = 0; i < samples; ++i) {
    const uint8_t *p = src + 2 * (size_t)i * (size_t)width;
    if (width == 2) {
      out[i].r = half_value(half_bits(p));
      out[i].i = half_value(half_bits(p + 2));
    } else if (width == 4) {
      float real, imaginary;
      memcpy(&real, p, 4);
      memcpy(&imaginary, p + 4, 4);
      out[i].r = real;
      out[i].i = imaginary;
    } else {
      memcpy(&out[i].r, p, 8);
      memcpy(&out[i].i, p + 8, 8);
    }
  }
}

static void set_dimensions(SEXP out, const rumi_layout *layout) {
  SEXP dims = PROTECT(Rf_allocVector(INTSXP, layout->ndim));
  for (int i = 0; i < layout->ndim; ++i) {
    int64_t value = layout->shape[layout->ndim - i - 1];
    if (value > INT_MAX)
      Rf_error("an output dimension exceeds R's integer limit");
    INTEGER(dims)[i] = (int)value;
  }
  Rf_setAttrib(out, R_DimSymbol, dims);
  UNPROTECT(1);
}

static SEXP decoded_array(SEXP bytes, rumi_dtype dtype,
                          const rumi_layout *layout) {
  const rumi_dtype_info *info = dtype_info(dtype);
  R_xlen_t samples = XLENGTH(bytes) / info->storage_bytes;
  SEXP out;
  int protect = 0;
  if (dtype == RUMI_DT_BOOL) {
    out = PROTECT(Rf_allocVector(LGLSXP, samples));
    ++protect;
    for (R_xlen_t i = 0; i < samples; ++i)
      LOGICAL(out)[i] = RAW(bytes)[i] != 0;
  } else if (dtype == RUMI_DT_UINT8 || dtype == RUMI_DT_INT8 ||
             dtype == RUMI_DT_UINT16 || dtype == RUMI_DT_INT16) {
    out = PROTECT(Rf_allocVector(INTSXP, samples));
    ++protect;
    integer_samples(INTEGER(out), RAW(bytes), dtype, samples);
  } else if (dtype == RUMI_DT_COMPLEX32 || dtype == RUMI_DT_COMPLEX64 ||
             dtype == RUMI_DT_COMPLEX128) {
    out = PROTECT(Rf_allocVector(CPLXSXP, samples));
    ++protect;
    complex_samples(COMPLEX(out), RAW(bytes), info->component_bytes, samples);
  } else if (dtype == RUMI_DT_FLOAT8_E4M3FN ||
             dtype == RUMI_DT_FLOAT8_E5M2 ||
             dtype == RUMI_DT_FLOAT8_E8M0FNU ||
             dtype == RUMI_DT_FLOAT8_E4M3FNUZ ||
             dtype == RUMI_DT_FLOAT8_E5M2FNUZ) {
    out = bytes;
  } else {
    out = PROTECT(Rf_allocVector(REALSXP, samples));
    ++protect;
    int inexact = real_samples(REAL(out), RAW(bytes), dtype, samples);
    if (inexact)
      Rf_warning("64-bit integer values above 2^53 lost precision as R doubles");
  }

  set_dimensions(out, layout);
  if (out == bytes) {
    SEXP cls = PROTECT(Rf_mkString("rumi_float8_array"));
    ++protect;
    Rf_classgets(out, cls);
  }
  UNPROTECT(protect);
  return out;
}

static void fits_r(size_t bytes) {
  if (bytes > (size_t)R_XLEN_T_MAX)
    Rf_error("Rumi output is too large for R");
}

SEXP rumi_r_read(SEXP pointer, SEXP time_s, SEXP bands_s,
                 SEXP window_s, SEXP pattern_s) {
  dataset *value = dataset_of(pointer);
  rumi_header h;
  fail(rumi_spec_header(value->spec, &h));
  int *times, *bands;
  size_t nt, nb;
  selection(time_s, "time", &times, &nt);
  selection(bands_s, "bands", &bands, &nb);
  int y_off, y_size, x_off, x_size;
  window(window_s, &h, &y_off, &y_size, &x_off, &x_size);
  const char *output_pattern = pattern(pattern_s);
  rumi_layout layout;
  size_t bytes_n;
  fail(rumi_read_layout(value->spec, times, nt, bands, nb, y_off, y_size,
                        x_off, x_size, output_pattern, &layout, &bytes_n));
  fits_r(bytes_n);
  int direct = h.dtype == RUMI_DT_FLOAT64;
  SEXP bytes = PROTECT(direct ?
      Rf_allocVector(REALSXP, (R_xlen_t)(bytes_n / sizeof(double))) :
      Rf_allocVector(RAWSXP, (R_xlen_t)bytes_n));
  void *buffer = direct ? (void *)REAL(bytes) : (void *)RAW(bytes);
  fail(rumi_read(value->source, value->spec, times, nt, bands, nb,
                 y_off, y_size, x_off, x_size, output_pattern,
                 buffer, bytes_n));
  if (direct) {
    set_dimensions(bytes, &layout);
    UNPROTECT(1);
    return bytes;
  }
  SEXP out = PROTECT(decoded_array(bytes, h.dtype, &layout));
  UNPROTECT(2);
  return out;
}

SEXP rumi_r_read_many(SEXP pointers, SEXP windows_s, SEXP time_s,
                      SEXP bands_s, SEXP pattern_s) {
  if (TYPEOF(pointers) != VECSXP || XLENGTH(pointers) == 0)
    Rf_error("`files` must not be empty");
  size_t n = (size_t)XLENGTH(pointers);
  if (TYPEOF(windows_s) != VECSXP || (size_t)XLENGTH(windows_s) != n)
    Rf_error("`windows` must have one entry per dataset");
  rumi_read_item *items = (rumi_read_item *)R_alloc(n, sizeof(*items));
  rumi_header first;
  int y_size = 0, x_size = 0;
  for (size_t i = 0; i < n; ++i) {
    dataset *value = dataset_of(VECTOR_ELT(pointers, i));
    rumi_header h;
    fail(rumi_spec_header(value->spec, &h));
    if (i == 0)
      first = h;
    int y_off, item_y, x_off, item_x;
    window(VECTOR_ELT(windows_s, i), &h, &y_off, &item_y, &x_off, &item_x);
    if (i == 0) {
      y_size = item_y;
      x_size = item_x;
    } else if (item_y != y_size || item_x != x_size) {
      Rf_error("every window must have the same height and width");
    }
    items[i] = (rumi_read_item){value->source, value->spec, y_off, x_off};
  }
  int *times, *bands;
  size_t nt, nb;
  selection(time_s, "time", &times, &nt);
  selection(bands_s, "bands", &bands, &nb);
  const char *output_pattern = pattern(pattern_s);
  rumi_layout layout;
  size_t bytes_n;
  fail(rumi_read_many_layout(items, n, times, nt, bands, nb, y_size, x_size,
                             output_pattern, &layout, &bytes_n));
  fits_r(bytes_n);
  int direct = first.dtype == RUMI_DT_FLOAT64;
  SEXP bytes = PROTECT(direct ?
      Rf_allocVector(REALSXP, (R_xlen_t)(bytes_n / sizeof(double))) :
      Rf_allocVector(RAWSXP, (R_xlen_t)bytes_n));
  void *buffer = direct ? (void *)REAL(bytes) : (void *)RAW(bytes);
  fail(rumi_read_many(items, n, times, nt, bands, nb, y_size, x_size,
                      output_pattern, buffer, bytes_n));
  if (direct) {
    set_dimensions(bytes, &layout);
    UNPROTECT(1);
    return bytes;
  }
  SEXP out = PROTECT(decoded_array(bytes, first.dtype, &layout));
  UNPROTECT(2);
  return out;
}

static size_t element_size(SEXP x) {
  switch (TYPEOF(x)) {
  case RAWSXP:
    return 1;
  case LGLSXP:
  case INTSXP:
    return sizeof(int);
  case REALSXP:
    return sizeof(double);
  case CPLXSXP:
    return sizeof(Rcomplex);
  default:
    Rf_error("`x` must be a raw, logical, integer, double or complex array");
  }
}

static uint8_t *elements(SEXP x) {
  switch (TYPEOF(x)) {
  case RAWSXP:
    return RAW(x);
  case LGLSXP:
    return (uint8_t *)LOGICAL(x);
  case INTSXP:
    return (uint8_t *)INTEGER(x);
  case REALSXP:
    return (uint8_t *)REAL(x);
  default:
    return (uint8_t *)COMPLEX(x);
  }
}

// R stores the pattern's axes, and each frame's axes, in reverse order.
SEXP rumi_r_frames(SEXP x, SEXP pattern_s, SEXP tile_s) {
  size_t width = element_size(x);
  rumi_frame_pattern p;
  fail(rumi_compile_frame_pattern(Rf_translateCharUTF8(STRING_ELT(pattern_s, 0)),
                                  &p));
  SEXP dims = Rf_getAttrib(x, R_DimSymbol);
  int ndim = Rf_isNull(dims) ? 1 : (int)XLENGTH(dims);
  if (ndim != p.input_ndim)
    Rf_error("the pattern names %d axes, but `x` has %d dimensions",
             p.input_ndim, ndim);

  // indexed by rumi_axis; an axis the pattern omits has extent one
  int64_t extent[6] = {1, 1, 1, 1, 1, 1};
  int64_t stride[6] = {0};
  int64_t step = 1;
  for (int i = p.input_ndim - 1; i >= 0; --i) {
    int64_t n = INTEGER(dims)[p.input_ndim - 1 - i];
    extent[p.input[i]] = n;
    stride[p.input[i]] = step;
    step *= n;
  }
  if (extent[RUMI_AXIS_BAND] > 65535)
    Rf_error("`x` has %lld bands, more than the 65535 a file holds",
             (long long)extent[RUMI_AXIS_BAND]);
  uint16_t bands = (uint16_t)extent[RUMI_AXIS_BAND];
  uint32_t times = (uint32_t)extent[RUMI_AXIS_TIME];
  uint32_t length = (uint32_t)extent[RUMI_AXIS_Y];
  uint32_t across = (uint32_t)extent[RUMI_AXIS_X];
  uint16_t tile = (uint16_t)INTEGER(tile_s)[0];

  uint8_t unit;
  fail(rumi_frame_unit(&p, bands, times, &unit));
  char layout[16] = {0};
  fail(rumi_unit_name(unit, bands, times, layout, sizeof layout));
  int held_b = strchr(layout, 'b') != NULL;
  int held_t = strchr(layout, 't') != NULL;
  uint64_t count;
  fail(rumi_frame_count(unit, across, length, tile, bands, times, NULL, NULL,
                        &count));
  if (count > (uint64_t)R_XLEN_T_MAX)
    Rf_error("`x` cuts into more frames than an R list holds");

  const uint8_t *src = elements(x);
  SEXP frames = PROTECT(Rf_allocVector(VECSXP, (R_xlen_t)count));
  for (uint64_t k = 0; k < count; ++k) {
    rumi_frame_at at;
    fail(rumi_frame_locate(unit, across, length, tile, bands, times, k, &at));
    // the cut is ordered as held b, held t, h, w, which perm starts from
    int64_t cut[RUMI_MAX_AXES];
    int n = 0;
    if (held_b)
      cut[n++] = stride[RUMI_AXIS_BAND];
    if (held_t)
      cut[n++] = stride[RUMI_AXIS_TIME];
    cut[n++] = stride[RUMI_AXIS_Y];
    cut[n++] = stride[RUMI_AXIS_X];
    if (n != at.ndim)
      Rf_error("librumi returned a frame of %d axes for layout %s", at.ndim,
               layout);
    int64_t base = (int64_t)at.row * tile * stride[RUMI_AXIS_Y] +
                   (int64_t)at.col * tile * stride[RUMI_AXIS_X];
    if (!held_b)
      base += (int64_t)at.band * stride[RUMI_AXIS_BAND];
    if (!held_t)
      base += (int64_t)at.time * stride[RUMI_AXIS_TIME];
    int64_t from[RUMI_MAX_AXES];
    R_xlen_t samples = 1;
    for (int d = 0; d < at.ndim; ++d) {
      from[d] = cut[at.perm[d]];
      samples *= (R_xlen_t)at.dims[d];
    }

    SEXP frame = PROTECT(Rf_allocVector(TYPEOF(x), samples));
    uint8_t *dst = elements(frame);
    int64_t index[RUMI_MAX_AXES] = {0};
    for (R_xlen_t i = 0; i < samples; ++i) {
      int64_t offset = base;
      for (int d = 0; d < at.ndim; ++d)
        offset += index[d] * from[d];
      memcpy(dst + (size_t)i * width, src + (size_t)offset * width, width);
      for (int d = at.ndim - 1; d >= 0; --d) {
        if (++index[d] < at.dims[d])
          break;
        index[d] = 0;
      }
    }
    SEXP frame_dims = PROTECT(Rf_allocVector(INTSXP, at.ndim));
    for (int d = 0; d < at.ndim; ++d)
      INTEGER(frame_dims)[d] = (int)at.dims[at.ndim - 1 - d];
    Rf_setAttrib(frame, R_DimSymbol, frame_dims);
    SET_VECTOR_ELT(frames, (R_xlen_t)k, frame);
    UNPROTECT(2);
  }

  static const char *const out_names[] = {
      "data", "layout", "frame_unit", "image_width", "image_length",
      "tile_size", "bands", "time_count"};
  SEXP out = PROTECT(Rf_allocVector(VECSXP, 8));
  SET_VECTOR_ELT(out, 0, frames);
  SET_VECTOR_ELT(out, 1, Rf_mkString(layout));
  SET_VECTOR_ELT(out, 2, Rf_ScalarInteger(unit));
  SET_VECTOR_ELT(out, 3, Rf_ScalarReal(across));
  SET_VECTOR_ELT(out, 4, Rf_ScalarReal(length));
  SET_VECTOR_ELT(out, 5, Rf_ScalarInteger(tile));
  SET_VECTOR_ELT(out, 6, Rf_ScalarInteger(bands));
  SET_VECTOR_ELT(out, 7, Rf_ScalarReal(times));
  names(out, out_names, 8);
  UNPROTECT(2);
  return out;
}

static rumi_dtype dtype_named(const char *name) {
  const rumi_dtype_info *registry = NULL;
  size_t count = rumi_dtype_registry(&registry);
  for (size_t i = 0; i < count; ++i)
    if (strcmp(registry[i].name, name) == 0)
      return (rumi_dtype)registry[i].code;
  Rf_error("`datatype` \"%s\" is not a Rumi sample type", name);
}

SEXP rumi_r_write(SEXP path_s, SEXP payloads_s, SEXP shape_s, SEXP dtype_s,
                  SEXP transform_s, SEXP epsg_s, SEXP point_s, SEXP texts_s,
                  SEXP time_type_s, SEXP time_s) {
  size_t count = (size_t)XLENGTH(payloads_s);
  const unsigned char **frames =
      (const unsigned char **)R_alloc(count, sizeof(*frames));
  size_t *sizes = (size_t *)R_alloc(count, sizeof(*sizes));
  static unsigned char empty;
  for (size_t i = 0; i < count; ++i) {
    SEXP payload = VECTOR_ELT(payloads_s, (R_xlen_t)i);
    if (TYPEOF(payload) != RAWSXP)
      Rf_error("frame %zu has no payload", i + 1);
    sizes[i] = (size_t)XLENGTH(payload);
    // Keep empty payloads distinct from null pointers.
    frames[i] = sizes[i] ? RAW(payload) : &empty;
  }

  const double *shape = REAL(shape_s);
  rumi_write_desc desc = {0};
  desc.image_width = (uint32_t)shape[0];
  desc.image_length = (uint32_t)shape[1];
  desc.time_count = (uint32_t)shape[2];
  desc.tile_size = (uint16_t)shape[3];
  desc.samples_per_pixel = (uint16_t)shape[4];
  desc.frame_unit = (uint8_t)shape[5];
  desc.dtype = dtype_named(Rf_translateCharUTF8(STRING_ELT(dtype_s, 0)));
  desc.transform = Rf_isNull(transform_s) ? NULL : REAL(transform_s);
  desc.epsg = Rf_isNull(epsg_s) ? 0 : (uint32_t)INTEGER(epsg_s)[0];
  desc.pixel_is_point = LOGICAL(point_s)[0];

  size_t ntexts = (size_t)XLENGTH(texts_s);
  const char **texts = (const char **)R_alloc(ntexts, sizeof(*texts));
  for (size_t i = 0; i < ntexts; ++i)
    texts[i] = Rf_translateCharUTF8(STRING_ELT(texts_s, (R_xlen_t)i));
  desc.band_texts = texts;
  desc.band_text_count = ntexts;

  size_t ncoords = (size_t)XLENGTH(time_s);
  int64_t *seconds = (int64_t *)R_alloc(ncoords, sizeof(*seconds));
  for (size_t i = 0; i < ncoords; ++i)
    seconds[i] = (int64_t)REAL(time_s)[i];
  desc.time_type = (uint8_t)INTEGER(time_type_s)[0];
  desc.time = seconds;
  desc.time_coords = ncoords;

  unsigned char *blob = NULL;
  size_t blob_size = 0;
  fail(rumi_write(Rf_translateCharUTF8(STRING_ELT(path_s, 0)), &desc, frames,
                  sizes, count, &blob, &blob_size));
  SEXP header = Rf_allocVector(RAWSXP, (R_xlen_t)blob_size);
  memcpy(RAW(header), blob, blob_size);
  rumi_free(blob);
  return header;
}

static const R_CallMethodDef calls[] = {
    {"rumi_r_open", (DL_FUNC)&rumi_r_open, 2},
    {"rumi_r_close", (DL_FUNC)&rumi_r_close, 1},
    {"rumi_r_read", (DL_FUNC)&rumi_r_read, 5},
    {"rumi_r_read_many", (DL_FUNC)&rumi_r_read_many, 5},
    {"rumi_r_frames", (DL_FUNC)&rumi_r_frames, 3},
    {"rumi_r_write", (DL_FUNC)&rumi_r_write, 10},
    {NULL, NULL, 0}};

void attribute_visible R_init_rumi(DllInfo *dll) {
  R_registerRoutines(dll, NULL, calls, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
}
