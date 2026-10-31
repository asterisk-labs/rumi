#include "common/simd.h"

#include <stdlib.h>
#include <string.h>

/* What the build carries. AVX2 is compiled through a target attribute, so on
   x86 it is there whether or not the unit was built with -mavx2. */
#define GEOZL_BUILT_SSE2 GEOZL_SIMD_X86
#define GEOZL_BUILT_NEON GEOZL_SIMD_HAS_NEON
#define GEOZL_BUILT_AVX2 GEOZL_SIMD_CAN_AVX2

unsigned geozl_simd_built(void) {
  unsigned m = GEOZL_SIMD_BIT(GEOZL_SIMD_SCALAR);
  if (GEOZL_BUILT_SSE2)
    m |= GEOZL_SIMD_BIT(GEOZL_SIMD_SSE2);
  if (GEOZL_BUILT_AVX2)
    m |= GEOZL_SIMD_BIT(GEOZL_SIMD_AVX2);
  if (GEOZL_BUILT_NEON)
    m |= GEOZL_SIMD_BIT(GEOZL_SIMD_NEON);
  return m;
}

static unsigned probe_cpu(void) {
  unsigned m = GEOZL_SIMD_BIT(GEOZL_SIMD_SCALAR);
#if defined(__aarch64__) || defined(_M_ARM64)
  m |= GEOZL_SIMD_BIT(GEOZL_SIMD_NEON); /* part of ARMv8 */
#elif defined(__x86_64__) || defined(_M_X64)
  m |= GEOZL_SIMD_BIT(GEOZL_SIMD_SSE2); /* part of the x86-64 baseline */
#if defined(__GNUC__) || defined(__clang__)
  if (__builtin_cpu_supports("avx2"))
    m |= GEOZL_SIMD_BIT(GEOZL_SIMD_AVX2);
#endif
#endif
  return m;
}

/* Not cached. geozl_simd_resolve caches its own answer and is the only caller
   on a hot path; the other is simd_info on the Python side, which asks once and
   is reporting. A cache here would only add a second racing write. */
unsigned geozl_simd_cpu(void) { return probe_cpu(); }

static int top_of(unsigned mask) {
  int best = GEOZL_SIMD_SCALAR;
  for (int p = GEOZL_SIMD_SCALAR; p <= GEOZL_SIMD_NEON; ++p)
    if (mask & GEOZL_SIMD_BIT(p))
      best = p;
  return best;
}

/* Paths GEOZL_SIMD leaves on the table. Unset, or set to something this build
   does not know, allows everything; NEON used to double as that sentinel, which
   meant GEOZL_SIMD=avx512 quietly meant no ceiling at all. */
#define GEOZL_SIMD_ALL_ALLOWED (~0u)

static unsigned allowed_from_env(void) {
  const char *v = getenv("GEOZL_SIMD");
  if (v == NULL || v[0] == '\0')
    return GEOZL_SIMD_ALL_ALLOWED;
  for (int p = GEOZL_SIMD_SCALAR; p <= GEOZL_SIMD_NEON; ++p)
    if (strcmp(v, geozl_simd_name(p)) == 0) {
      /* Everything at or below the named path. The codes are not one ladder
         across ISAs, but within a build only one ISA family is ever present,
         so a ceiling inside that family is well defined. */
      unsigned m = 0;
      for (int q = GEOZL_SIMD_SCALAR; q <= p; ++q)
        m |= GEOZL_SIMD_BIT(q);
      return m;
    }
  return GEOZL_SIMD_ALL_ALLOWED;
}

#if defined(__STDC_NO_ATOMICS__)
int geozl_simd_path = -1;
#define GEOZL_SIMD_STORE(v, x) ((v) = (x))
#else
_Atomic int geozl_simd_path = -1;
#define GEOZL_SIMD_STORE(v, x)                                                 \
  atomic_store_explicit(&(v), (x), memory_order_relaxed)
#endif

int geozl_simd_resolve(void) {
  const int p =
      top_of(geozl_simd_built() & geozl_simd_cpu() & allowed_from_env());
  /* Several threads can land here at once. They all compute the same value, so
     the store is relaxed, but it has to be an atomic store all the same. */
  GEOZL_SIMD_STORE(geozl_simd_path, p);
  return p;
}

int geozl_simd_active(void) { return geozl_simd_now(); }

const char *geozl_simd_name(int path) {
  switch (path) {
  case GEOZL_SIMD_SCALAR: return "scalar";
  case GEOZL_SIMD_SSE2:   return "sse2";
  case GEOZL_SIMD_AVX2:   return "avx2";
  case GEOZL_SIMD_NEON:   return "neon";
  default:                return "unknown";
  }
}
