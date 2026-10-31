/* Which vector paths a build carries, which the machine can run, and which one
   actually runs. The three differ, and only the gap between them shows that a
   build lost a fast path, since the frames come out the same either way.

   The values cross into Python, so a published number keeps its meaning. */

#ifndef GEOZL_COMMON_SIMD_H
#define GEOZL_COMMON_SIMD_H

#define GEOZL_SIMD_SCALAR 0
#define GEOZL_SIMD_SSE2 1
#define GEOZL_SIMD_AVX2 2
#define GEOZL_SIMD_NEON 3

#define GEOZL_SIMD_BIT(p) (1u << (p))

/* An x86 wheel is built for the base ISA, so -mavx2 is not available as a build
   flag and the AVX2 kernels would simply not exist. A target attribute puts
   both in the same object and costs nothing, measured identical to a global
   -mavx2 build. */
#ifndef GEOZL_NO_SIMD
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__) ||             \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <immintrin.h>
#define GEOZL_SIMD_X86 1
#if defined(__GNUC__) || defined(__clang__)
#define GEOZL_SIMD_DISPATCH 1
#define GEOZL_TARGET_AVX2 __attribute__((target("avx2")))
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#include <arm_neon.h>
#define GEOZL_SIMD_HAS_NEON 1
#endif
#endif

#ifndef GEOZL_SIMD_X86
#define GEOZL_SIMD_X86 0
#endif
#ifndef GEOZL_SIMD_HAS_NEON
#define GEOZL_SIMD_HAS_NEON 0
#endif
#ifndef GEOZL_SIMD_DISPATCH
/* No attribute, so AVX2 only exists when the whole unit was built for it. */
#define GEOZL_SIMD_DISPATCH 0
#define GEOZL_TARGET_AVX2
#endif

/* Whether an AVX2 kernel can be compiled at all, which is the question every
   kernel asks before offering one. */
#if GEOZL_SIMD_X86 && (GEOZL_SIMD_DISPATCH || defined(__AVX2__))
#define GEOZL_SIMD_CAN_AVX2 1
#else
#define GEOZL_SIMD_CAN_AVX2 0
#endif

/* Resolved once and read inline, because the kernels ask per row.
   Relaxed atomic: several threads can race to resolve, they all compute the
   same value, but a plain int would still be a data race under C11. */
#if defined(__STDC_NO_ATOMICS__)
extern int geozl_simd_path;
#define GEOZL_SIMD_LOAD(v) (v)
#else
#include <stdatomic.h>
extern _Atomic int geozl_simd_path;
#define GEOZL_SIMD_LOAD(v) atomic_load_explicit(&(v), memory_order_relaxed)
#endif
int geozl_simd_resolve(void);

static inline int geozl_simd_now(void) {
  const int p = GEOZL_SIMD_LOAD(geozl_simd_path);
  return p >= 0 ? p : geozl_simd_resolve();
}

/* The codes are wire values that cross into Python, so they cannot be reordered
   to make AVX2 and NEON comparable. They are not one ladder anyway, NEON is a
   different ISA and not a step above AVX2. Ask for a path by name. */
static inline int geozl_simd_has(int path) { return geozl_simd_now() == path; }

unsigned geozl_simd_built(void);
unsigned geozl_simd_cpu(void);

/* Best path that is both built and available, lowered by GEOZL_SIMD when that
   names a narrower one. A value it does not recognise is ignored. This is the
   name the Python side reports under; geozl_simd_now is the one the kernels
   call. */
int geozl_simd_active(void);

const char *geozl_simd_name(int path);

#endif /* GEOZL_COMMON_SIMD_H */
