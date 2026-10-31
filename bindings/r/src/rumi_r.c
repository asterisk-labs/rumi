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

static void selection(SEXP value, const char *name, uint32_t total,
                      int **out, size_t *count) {
  if (Rf_isNull(value)) {
    *out = NULL;
    *count = 0;
    return;
  }
  if (TYPEOF(value) != INTSXP || XLENGTH(value) == 0)
    Rf_error("`%s` must be a non-empty integer vector or NULL", name);
  *count = (size_t)XLENGTH(value);
  *out = (int *)R_alloc(*count, sizeof(int));
  for (size_t i = 0; i < *count; ++i) {
    int index = INTEGER(value)[i];
    if (index < 1 || (uint32_t)index > total)
      Rf_error("`%s` index %d is outside 1:%u", name, index, total);
    (*out)[i] = index;
  }
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
  double row = REAL(value)[0], col = REAL(value)[1];
  double height = REAL(value)[2], width = REAL(value)[3];
  if (row < 1 || col < 1 || height < 1 || width < 1 ||
      row + height - 1 > h->image_length || col + width - 1 > h->image_width)
    Rf_error("`window` extends past the image");
  *y_off = (int)row - 1;
  *x_off = (int)col - 1;
  *y_size = (int)height;
  *x_size = (int)width;
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

static size_t output_bytes(const rumi_layout *layout, size_t width) {
  size_t size = width;
  for (int i = 0; i < layout->ndim; ++i) {
    if ((uint64_t)layout->shape[i] > SIZE_MAX / size)
      Rf_error("Rumi output is too large");
    size *= (size_t)layout->shape[i];
  }
  if (size > (size_t)R_XLEN_T_MAX)
    Rf_error("Rumi output is too large for R");
  return size;
}

SEXP rumi_r_read(SEXP pointer, SEXP time_s, SEXP bands_s,
                 SEXP window_s, SEXP pattern_s) {
  dataset *value = dataset_of(pointer);
  rumi_header h;
  fail(rumi_spec_header(value->spec, &h));
  int *times, *bands;
  size_t nt, nb;
  selection(time_s, "time", h.time_count, &times, &nt);
  selection(bands_s, "bands", h.samples_per_pixel, &bands, &nb);
  int y_off, y_size, x_off, x_size;
  window(window_s, &h, &y_off, &y_size, &x_off, &x_size);
  const char *output_pattern = pattern(pattern_s);
  if (output_pattern == NULL)
    output_pattern = rumi_default_pattern(1, h.time_count);
  rumi_layout layout;
  fail(rumi_compile_layout(output_pattern, 1,
      nt == 0 ? h.time_count : (int64_t)nt,
      nb == 0 ? h.samples_per_pixel : (int64_t)nb,
      y_size, x_size, &layout));
  size_t bytes_n = output_bytes(&layout, dtype_info(h.dtype)->storage_bytes);
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
  selection(time_s, "time", first.time_count, &times, &nt);
  selection(bands_s, "bands", first.samples_per_pixel, &bands, &nb);
  const char *output_pattern = pattern(pattern_s);
  if (output_pattern == NULL)
    output_pattern = rumi_default_pattern(n, first.time_count);
  rumi_layout layout;
  fail(rumi_compile_layout(output_pattern, (int64_t)n,
      nt == 0 ? first.time_count : (int64_t)nt,
      nb == 0 ? first.samples_per_pixel : (int64_t)nb,
      y_size, x_size, &layout));
  size_t bytes_n = output_bytes(&layout, dtype_info(first.dtype)->storage_bytes);
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

static const R_CallMethodDef calls[] = {
    {"rumi_r_open", (DL_FUNC)&rumi_r_open, 2},
    {"rumi_r_close", (DL_FUNC)&rumi_r_close, 1},
    {"rumi_r_read", (DL_FUNC)&rumi_r_read, 5},
    {"rumi_r_read_many", (DL_FUNC)&rumi_r_read_many, 5},
    {NULL, NULL, 0}};

void attribute_visible R_init_rumi(DllInfo *dll) {
  R_registerRoutines(dll, NULL, calls, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  R_forceSymbols(dll, TRUE);
}
