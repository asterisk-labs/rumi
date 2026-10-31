// KEY=VALUE fields, the grammar the three error recipes share.

#ifndef GEOZL_COMMON_RECIPE_PARSE_H
#define GEOZL_COMMON_RECIPE_PARSE_H

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int geozl_recipe_fail(char *err, size_t errSize, const char *fmt,
                                    ...) {
  if (err != NULL && errSize != 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errSize, fmt, ap);
    va_end(ap);
  }
  return 1;
}

// Consumed whole, so "1.0.0" fails instead of reading as 1.0. A recipe writes a
// dot and strtod reads whatever LC_NUMERIC declares, so the dot is translated
// rather than the conversion replaced, which keeps it correctly rounded.
//
// Every character can be a dot and each one grows to plen bytes, so the bound is
// on n * plen. ERANGE also refuses an underflow to subnormal, which resolve
// cannot cut a grid from.
static inline int geozl_recipe_number(const char *s, const char *end,
                                      double *out) {
  const char *point = localeconv()->decimal_point;
  if (point == NULL || point[0] == '\0')
    point = ".";
  const size_t plen = strlen(point);

  char buf[64];
  const size_t n = (size_t)(end - s);
  if (n == 0 || n >= sizeof(buf) / plen)
    return 1;

  size_t w = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '.') {
      memcpy(buf + w, point, plen);
      w += plen;
    } else {
      buf[w++] = s[i];
    }
  }
  buf[w] = '\0';

  char *tail = NULL;
  errno = 0;
  const double v = strtod(buf, &tail);
  if (tail != buf + w || errno == ERANGE || !isfinite(v))
    return 1;
  *out = v;
  return 0;
}

static inline const char *geozl_recipe_field_end(const char *s) {
  while (*s != '\0' && *s != ',')
    ++s;
  return s;
}

static inline int geozl_recipe_keyed(const char **cur, const char *name,
                                     const char **vbeg, const char **vend) {
  const char *t = *cur;
  const size_t n = strlen(name);
  if (strncmp(t, name, n) != 0 || t[n] != '=')
    return 1;
  *vbeg = t + n + 1;
  *vend = geozl_recipe_field_end(*vbeg);
  *cur = *vend;
  return 0;
}

#endif // GEOZL_COMMON_RECIPE_PARSE_H
