#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>

#include "rumi/rumi.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static void fail(rumi_status status) {
  if (status == RUMI_OK)
    return;
  const char *message = rumi_last_error();
  Rf_error("%s", message == NULL ? "librumi failed" : message);
}

static rumi_dtype dtype_arg(SEXP s, size_t *width) {
  if (!Rf_isString(s) || XLENGTH(s) != 1 || STRING_ELT(s, 0) == NA_STRING)
    Rf_error("`datatype` must be one sample type such as \"uint16\"");
  const char *name = Rf_translateCharUTF8(STRING_ELT(s, 0));
  const rumi_dtype_info *registry = NULL;
  size_t count = rumi_dtype_registry(&registry);
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(registry[i].name, name) == 0) {
      *width = registry[i].storage_bytes;
      return (rumi_dtype)registry[i].code;
    }
  }
  Rf_error("`datatype` is not a Rumi sample type");
}

static int is_int_type(rumi_dtype d) {
  return d >= RUMI_DT_UINT8 && d <= RUMI_DT_INT64;
}

static int packs(rumi_dtype d) {
  return is_int_type(d) || d == RUMI_DT_FLOAT16 || d == RUMI_DT_FLOAT32 ||
         d == RUMI_DT_FLOAT64 || d == RUMI_DT_BOOL;
}

// Exclusive upper bounds avoid rounding the 64-bit maxima up as doubles.
static void int_range(rumi_dtype d, double *lo, double *hiExcl) {
  switch (d) {
  case RUMI_DT_UINT8: *lo = 0; *hiExcl = 256.0; break;
  case RUMI_DT_UINT16: *lo = 0; *hiExcl = 65536.0; break;
  case RUMI_DT_UINT32: *lo = 0; *hiExcl = 4294967296.0; break;
  case RUMI_DT_UINT64: *lo = 0; *hiExcl = 18446744073709551616.0; break;
  case RUMI_DT_INT8: *lo = -128.0; *hiExcl = 128.0; break;
  case RUMI_DT_INT16: *lo = -32768.0; *hiExcl = 32768.0; break;
  case RUMI_DT_INT32: *lo = -2147483648.0; *hiExcl = 2147483648.0; break;
  default: *lo = -9223372036854775808.0; *hiExcl = 9223372036854775808.0; break;
  }
}

static int int_fits(rumi_dtype d, double v) {
  double lo, hi;
  int_range(d, &lo, &hi);
  return R_FINITE(v) && v == floor(v) && v >= lo && v < hi;
}

// Round to IEEE half; report finite values beyond its range.
static uint16_t f64_to_f16(double v, int *overflow) {
  *overflow = 0;
  uint16_t sign = signbit(v) ? 0x8000u : 0;
  double a = fabs(v);
  if (ISNAN(v))
    return 0x7E00u;
  if (!R_FINITE(v))
    return sign | 0x7C00u;
  if (a < ldexp(1.0, -14)) // subnormal: units of 2^-24
    return sign | (uint16_t)nearbyint(ldexp(a, 24));
  int e;
  frexp(a, &e);
  e -= 1; // a = s * 2^e with s in [1, 2)
  double q = nearbyint(ldexp(a, 10 - e));
  if (q == 2048.0) {
    q = 1024.0;
    e += 1;
  }
  if (e > 15) {
    *overflow = 1;
    return sign | 0x7C00u;
  }
  return sign | (uint16_t)((e + 15) << 10) | (uint16_t)(q - 1024.0);
}

static void store(uint8_t *p, rumi_dtype d, double v) {
  switch (d) {
  case RUMI_DT_UINT8: case RUMI_DT_BOOL: *p = (uint8_t)v; break;
  case RUMI_DT_INT8: *(int8_t *)p = (int8_t)v; break;
  case RUMI_DT_UINT16: { uint16_t t = (uint16_t)v; memcpy(p, &t, 2); break; }
  case RUMI_DT_INT16: { int16_t t = (int16_t)v; memcpy(p, &t, 2); break; }
  case RUMI_DT_UINT32: { uint32_t t = (uint32_t)v; memcpy(p, &t, 4); break; }
  case RUMI_DT_INT32: { int32_t t = (int32_t)v; memcpy(p, &t, 4); break; }
  case RUMI_DT_UINT64: { uint64_t t = (uint64_t)v; memcpy(p, &t, 8); break; }
  case RUMI_DT_INT64: { int64_t t = (int64_t)v; memcpy(p, &t, 8); break; }
  case RUMI_DT_FLOAT16: { int o; uint16_t t = f64_to_f16(v, &o); memcpy(p, &t, 2); break; }
  case RUMI_DT_FLOAT32: { float t = (float)v; memcpy(p, &t, 4); break; }
  default: memcpy(p, &v, 8); break;
  }
}

static void check_value(rumi_dtype d, double v, const char *what) {
  if (d == RUMI_DT_BOOL) {
    if (v != 0 && v != 1)
      Rf_error("%s is not TRUE or FALSE", what);
  } else if (is_int_type(d)) {
    if (!int_fits(d, v))
      Rf_error("%s does not fit the integer datatype", what);
  } else if (d == RUMI_DT_FLOAT32) {
    if (R_FINITE(v) && fabs(v) > FLT_MAX)
      Rf_error("%s is outside the float32 range", what);
  } else if (d == RUMI_DT_FLOAT16) {
    int overflow;
    f64_to_f16(v, &overflow);
    if (overflow)
      Rf_error("%s is outside the float16 range", what);
  }
}

// Preserve float64 NaN payloads when no sentinel is given.
SEXP rumi_r_geozl_pack(SEXP x, SEXP dtype_s, SEXP nodata_s) {
  size_t w;
  rumi_dtype d = dtype_arg(dtype_s, &w);
  if (TYPEOF(x) == RAWSXP) {
    if ((size_t)XLENGTH(x) % w != 0)
      Rf_error("a raw `x` must hold whole samples of the datatype");
    return x;
  }
  if (TYPEOF(x) != INTSXP && TYPEOF(x) != REALSXP && TYPEOF(x) != LGLSXP)
    Rf_error("`x` must be a double, integer, logical or raw vector");
  if (!packs(d))
    Rf_error("this datatype takes `x` as raw samples");
  int hasNodata = !Rf_isNull(nodata_s);
  double nodata = hasNodata ? Rf_asReal(nodata_s) : 0.0;
  R_xlen_t n = XLENGTH(x);
  SEXP out = PROTECT(Rf_allocVector(RAWSXP, n * (R_xlen_t)w));
  uint8_t *dst = RAW(out);
  const int *ix = TYPEOF(x) == REALSXP ? NULL
                  : TYPEOF(x) == INTSXP ? INTEGER(x) : LOGICAL(x);
  const double *dx = ix ? NULL : REAL(x);
  for (R_xlen_t i = 0; i < n; ++i) {
    double v;
    int missing;
    if (ix) {
      missing = ix[i] == NA_INTEGER;
      v = missing ? NA_REAL : ix[i];
    } else {
      v = dx[i];
      missing = ISNAN(v);
    }
    if (missing) {
      if (hasNodata)
        v = nodata;
      else if (is_int_type(d) || d == RUMI_DT_BOOL)
        Rf_error("`x` has NA; an integer datatype needs `nodata` to mark it");
    }
    check_value(d, v, "a value of `x`");
    store(dst + (size_t)i * w, d, v);
  }
  UNPROTECT(1);
  return out;
}

SEXP rumi_r_geozl_itemsize(SEXP dtype_s) {
  size_t w;
  dtype_arg(dtype_s, &w);
  return Rf_ScalarInteger((int)w);
}

static void graph_finalizer(SEXP ptr) {
  rumi_geozl_graph_close(R_ExternalPtrAddr(ptr));
  R_ClearExternalPtr(ptr);
}

static rumi_geozl_graph *graph_of(SEXP ptr) {
  if (TYPEOF(ptr) != EXTPTRSXP ||
      R_ExternalPtrTag(ptr) != Rf_install("rumi_geozl_graph"))
    Rf_error("`graph` must come from rumi_geozl_graph()");
  rumi_geozl_graph *g = R_ExternalPtrAddr(ptr);
  if (g == NULL)
    Rf_error("this graph is no longer open; a saved graph does not survive a "
             "new session, build it again with rumi_geozl_graph()");
  return g;
}

static uint32_t count_arg(SEXP s, const char *message) {
  double v = Rf_isNumeric(s) && XLENGTH(s) == 1 ? Rf_asReal(s) : NA_REAL;
  if (!R_FINITE(v) || v < 0 || v > 4294967295.0 || v != floor(v))
    Rf_error("%s", message);
  return (uint32_t)v;
}

// OpenZL needs a non-null pointer even for an empty stream.
static const void *raw_samples(SEXP bytes) {
  static const uint64_t empty = 0;
  return XLENGTH(bytes) ? (const void *)RAW(bytes) : &empty;
}

SEXP rumi_r_geozl_graph(SEXP bytes, SEXP method_s, SEXP width_s, SEXP planes_s,
                        SEXP error_s, SEXP dtype_s, SEXP nodata_s) {
  if (!Rf_isString(method_s) || XLENGTH(method_s) != 1 ||
      STRING_ELT(method_s, 0) == NA_STRING)
    Rf_error("`method` must be a recipe such as \"planar>zigzag>pfor\"");
  if (!Rf_isNull(error_s) && (!Rf_isString(error_s) || XLENGTH(error_s) != 1))
    Rf_error("`error` must be a single number, percentage or recipe");
  size_t w;
  rumi_dtype d = dtype_arg(dtype_s, &w);
  uint32_t width = count_arg(width_s, "`width` must be a single whole number");
  uint32_t planes = count_arg(planes_s, "`planes` must be a single whole number");
  const char *error = Rf_isNull(error_s) ? NULL : Rf_translateCharUTF8(STRING_ELT(error_s, 0));

  // The core rejects sentinels for unsupported types; the sample is unused.
  uint8_t sample[8] = {0};
  const uint8_t *nodata = NULL;
  if (!Rf_isNull(nodata_s)) {
    double v = Rf_asReal(nodata_s);
    if (packs(d) && d != RUMI_DT_BOOL) {
      if (ISNAN(v) && is_int_type(d))
        Rf_error("an integer raster cannot use NaN as nodata");
      if (!ISNAN(v))
        check_value(d, v, "`nodata`");
      store(sample, d, v);
    }
    nodata = sample;
  }

  // Register cleanup before opening the native graph.
  SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, Rf_install("rumi_geozl_graph"), R_NilValue));
  R_RegisterCFinalizerEx(ptr, graph_finalizer, TRUE);
  rumi_geozl_graph *g = NULL;
  fail(rumi_geozl_graph_open(Rf_translateCharUTF8(STRING_ELT(method_s, 0)), d,
                             raw_samples(bytes), (size_t)XLENGTH(bytes), width,
                             planes, error, nodata, &g));
  R_SetExternalPtrAddr(ptr, g);
  UNPROTECT(1);
  return ptr;
}

typedef struct {
  unsigned char *frame;
  size_t size;
} frame_copy;

static SEXP frame_to_r(void *data) {
  frame_copy *c = data;
  SEXP out = Rf_allocVector(RAWSXP, (R_xlen_t)c->size);
  memcpy(RAW(out), c->frame, c->size);
  return out;
}

static void frame_free(void *data, Rboolean jump) {
  (void)jump;
  rumi_free(((frame_copy *)data)->frame);
}

SEXP rumi_r_geozl_compress(SEXP ptr, SEXP bytes, SEXP dtype_s) {
  rumi_geozl_graph *g = graph_of(ptr);
  size_t w;
  rumi_dtype d = dtype_arg(dtype_s, &w);
  frame_copy c = {NULL, 0};
  fail(rumi_geozl_compress(g, d, raw_samples(bytes), (size_t)XLENGTH(bytes),
                           &c.frame, &c.size));
  // Free the native frame if the R copy raises.
  return R_UnwindProtect(frame_to_r, &c, frame_free, &c, NULL);
}
