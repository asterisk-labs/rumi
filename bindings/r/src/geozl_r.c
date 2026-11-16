#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>

#include "rumi/rumi.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void rumi_r_raise(rumi_status status, const char *message);

// A bad argument, without a call, as stop(call. = FALSE) gives in the R code.
// The texts mirror the geozl R package.
#define bad(...) Rf_errorcall(R_NilValue, __VA_ARGS__)

static void fail(rumi_status status) {
  if (status == RUMI_OK)
    return;
  const char *message = rumi_last_error();
  char copy[1024];
  snprintf(copy, sizeof copy, "%s",
           message == NULL ? "librumi failed" : message);
  rumi_r_raise(status, copy);
}

// The R code resolves names and aliases, so only a canonical name arrives.
static const rumi_dtype_info *dtype_arg(SEXP s) {
  if (!Rf_isString(s) || XLENGTH(s) != 1 || STRING_ELT(s, 0) == NA_STRING)
    bad("`dtype` must be a single string");
  const char *name = Rf_translateCharUTF8(STRING_ELT(s, 0));
  const rumi_dtype_info *registry = NULL;
  size_t count = rumi_dtype_registry(&registry);
  for (size_t i = 0; i < count; ++i)
    if (strcmp(registry[i].name, name) == 0)
      return registry + i;
  bad("unknown dtype \"%s\"", name);
}

// Storage bytes per sample, named by every Rumi sample type.
SEXP rumi_r_dtype_sizes(void) {
  const rumi_dtype_info *registry = NULL;
  size_t count = rumi_dtype_registry(&registry);
  SEXP out = PROTECT(Rf_allocVector(INTSXP, (R_xlen_t)count));
  SEXP names = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t)count));
  for (size_t i = 0; i < count; ++i) {
    INTEGER(out)[i] = (int)registry[i].storage_bytes;
    SET_STRING_ELT(names, (R_xlen_t)i, Rf_mkChar(registry[i].name));
  }
  Rf_setAttrib(out, R_NamesSymbol, names);
  UNPROTECT(2);
  return out;
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

// Fails unless v, sample i of x, can be stored as d. Floats round to nearest.
static void check_value(const rumi_dtype_info *info, double v, R_xlen_t i) {
  rumi_dtype d = (rumi_dtype)info->code;
  if (d == RUMI_DT_BOOL) {
    if (v != 0 && v != 1)
      bad("value %.17g at position %.0f is not TRUE or FALSE", v,
          (double)i + 1);
  } else if (is_int_type(d)) {
    if (!int_fits(d, v))
      bad("value %.17g at position %.0f does not fit %s", v, (double)i + 1,
          info->name);
  } else if (d == RUMI_DT_FLOAT32) {
    if (R_FINITE(v) && fabs(v) > FLT_MAX)
      bad("value %.17g at position %.0f is outside the float32 range", v,
          (double)i + 1);
  } else if (d == RUMI_DT_FLOAT16) {
    int overflow;
    f64_to_f16(v, &overflow);
    if (overflow)
      bad("value %.17g at position %.0f is outside the float16 range", v,
          (double)i + 1);
  }
}

// Preserve float64 NaN payloads when no sentinel is given.
SEXP rumi_r_geozl_pack(SEXP x, SEXP dtype_s, SEXP nodata_s) {
  const rumi_dtype_info *info = dtype_arg(dtype_s);
  rumi_dtype d = (rumi_dtype)info->code;
  size_t w = info->storage_bytes;
  if (TYPEOF(x) == RAWSXP) {
    if ((size_t)XLENGTH(x) % w != 0)
      bad("a raw vector of %s samples needs a length divisible by %d",
          info->name, (int)w);
    return x;
  }
  if (TYPEOF(x) != INTSXP && TYPEOF(x) != REALSXP && TYPEOF(x) != LGLSXP)
    bad("`x` must be a double, integer, logical or raw vector, not %s",
        Rf_type2char(TYPEOF(x)));
  if (!packs(d))
    bad("%s samples take `x` as a raw vector", info->name);
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
      else if (d == RUMI_DT_BOOL)
        bad("`x` has NA at position %.0f, which a bool dtype cannot hold",
            (double)i + 1);
      else if (is_int_type(d))
        bad("`x` has NA at position %.0f; an integer dtype needs `nodata` "
            "to mark it", (double)i + 1);
    }
    check_value(info, v, i);
    store(dst + (size_t)i * w, d, v);
  }
  UNPROTECT(1);
  return out;
}

static void graph_finalizer(SEXP ptr) {
  rumi_geozl_graph_close(R_ExternalPtrAddr(ptr));
  R_ClearExternalPtr(ptr);
}

static rumi_geozl_graph *graph_of(SEXP ptr) {
  if (TYPEOF(ptr) != EXTPTRSXP ||
      R_ExternalPtrTag(ptr) != Rf_install("rumi_geozl_graph"))
    bad("`graph` must come from rumi_geozl_graph()");
  rumi_geozl_graph *g = R_ExternalPtrAddr(ptr);
  if (g == NULL)
    bad("this graph is no longer open; a saved graph does not survive a "
        "new session, build it again with rumi_geozl_graph()");
  return g;
}

static uint32_t u32_arg(SEXP s, const char *name) {
  double v = Rf_isNumeric(s) && XLENGTH(s) == 1 ? Rf_asReal(s) : NA_REAL;
  if (!R_FINITE(v) || v < 1 || v > 4294967295.0 || v != floor(v))
    bad("`%s` must be a whole number from 1 to 4294967295", name);
  return (uint32_t)v;
}

static const char *string_arg(SEXP s, const char *name) {
  if (!Rf_isString(s) || XLENGTH(s) != 1 || STRING_ELT(s, 0) == NA_STRING)
    bad("`%s` must be a single string", name);
  return Rf_translateCharUTF8(STRING_ELT(s, 0));
}

// OpenZL needs a non-null pointer even for an empty stream.
static const void *raw_samples(SEXP bytes) {
  static const uint64_t empty = 0;
  return XLENGTH(bytes) ? (const void *)RAW(bytes) : &empty;
}

SEXP rumi_r_geozl_graph(SEXP bytes, SEXP method_s, SEXP width_s, SEXP planes_s,
                        SEXP error_s, SEXP dtype_s, SEXP nodata_s) {
  const rumi_dtype_info *info = dtype_arg(dtype_s);
  rumi_dtype d = (rumi_dtype)info->code;
  const char *method = string_arg(method_s, "method");
  const char *error = Rf_isNull(error_s) ? NULL : string_arg(error_s, "error");
  uint32_t width = u32_arg(width_s, "width");
  uint32_t planes = u32_arg(planes_s, "planes");

  // The core rejects sentinels for unsupported types; the sample is unused.
  uint8_t sample[8] = {0};
  const uint8_t *nodata = NULL;
  if (!Rf_isNull(nodata_s)) {
    double v = Rf_asReal(nodata_s);
    if (packs(d) && d != RUMI_DT_BOOL) {
      // The core sees only the stored sample, so the range is checked here.
      if (ISNAN(v) && is_int_type(d))
        bad("a %s raster cannot use NaN as nodata", info->name);
      int outside = is_int_type(d) ? !int_fits(d, v)
                    : d == RUMI_DT_FLOAT32 && R_FINITE(v) && fabs(v) > FLT_MAX;
      if (!ISNAN(v) && outside)
        bad("nodata %.17g does not fit %s", v, info->name);
      store(sample, d, v);
    }
    nodata = sample;
  }

  // Register cleanup before opening the native graph.
  SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, Rf_install("rumi_geozl_graph"), R_NilValue));
  R_RegisterCFinalizerEx(ptr, graph_finalizer, TRUE);
  rumi_geozl_graph *g = NULL;
  fail(rumi_geozl_graph_open(method, d, raw_samples(bytes),
                             (size_t)XLENGTH(bytes), width, planes, error,
                             nodata, &g));
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

// The normalized error recipe, or NULL for lossless.
SEXP rumi_r_geozl_error(SEXP ptr) {
  const char *recipe = rumi_geozl_graph_error(graph_of(ptr));
  return recipe == NULL ? R_NilValue : Rf_mkString(recipe);
}

SEXP rumi_r_geozl_compress(SEXP ptr, SEXP bytes, SEXP dtype_s) {
  rumi_geozl_graph *g = graph_of(ptr);
  rumi_dtype d = (rumi_dtype)dtype_arg(dtype_s)->code;
  frame_copy c = {NULL, 0};
  fail(rumi_geozl_compress(g, d, raw_samples(bytes), (size_t)XLENGTH(bytes),
                           &c.frame, &c.size));
  // Free the native frame if the R copy raises.
  return R_UnwindProtect(frame_to_r, &c, frame_free, &c, NULL);
}
