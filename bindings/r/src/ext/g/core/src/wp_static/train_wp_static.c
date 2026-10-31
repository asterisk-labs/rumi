// Fit the wp_static kernel from a tile: ridge normal equations for the
// coefficients, then the shift with the lowest residual byte-plane entropy.
// Candidate 0 is planar and the winner is by argmin, so the fit never loses to
// planar; and since encode and decode apply the same K, a poor fit only costs
// ratio, never correctness. The 1 and 2 byte paths are unchanged. For 4 and 8
// byte samples the normal equations are scaled by the largest neighbour, which
// leaves the least-squares minimiser unchanged (scaling both sides of the fit
// by one constant) while keeping the products exact in double.

#include "train_wp_static.h"

#include "common/raster.h" // geozl_row_width

#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define WP_SHIFT_MAX 8
#define WP_MAX_CAND (WP_SHIFT_MAX + 2)
#define WP_MAX_PLANES 8 // a 64 bit sample splits into eight byte planes

static int solve4(double A[4][4], double b[4], double x[4]) {
  for (int col = 0; col < 4; ++col) {
    int piv = col;
    double best = fabs(A[col][col]);
    for (int r = col + 1; r < 4; ++r) {
      double v = fabs(A[r][col]);
      if (v > best) {
        best = v;
        piv = r;
      }
    }
    if (best < 1e-12)
      return 0;
    if (piv != col) {
      for (int k = 0; k < 4; ++k) {
        double t = A[col][k];
        A[col][k] = A[piv][k];
        A[piv][k] = t;
      }
      double t = b[col];
      b[col] = b[piv];
      b[piv] = t;
    }
    for (int r = col + 1; r < 4; ++r) {
      double m = A[r][col] / A[col][col];
      for (int k = col; k < 4; ++k)
        A[r][k] -= m * A[col][k];
      b[r] -= m * b[col];
    }
  }
  for (int r = 3; r >= 0; --r) {
    double s = b[r];
    for (int k = r + 1; k < 4; ++k)
      s -= A[r][k] * x[k];
    x[r] = s / A[r][r];
  }
  return 1;
}

// Largest neighbour magnitude, to scale the normal equations. Zero maps to one.
#define WP_MAXABS(T)                                                           \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    for (size_t r = 2; r < rows; ++r) {                                        \
      const T *up = s + (r - 1) * w;                                           \
      const T *up2 = s + (r - 2) * w;                                          \
      const T *row = s + r * w;                                                \
      for (size_t c = 1; c + 1 < w; ++c) {                                     \
        double m = (double)up[c];                                              \
        if ((double)up[c - 1] > m)                                             \
          m = (double)up[c - 1];                                               \
        if ((double)up[c + 1] > m)                                             \
          m = (double)up[c + 1];                                               \
        if ((double)up2[c] > m)                                                \
          m = (double)up2[c];                                                  \
        if ((double)row[c] > m)                                                \
          m = (double)row[c];                                                  \
        if (m > maxabs)                                                        \
          maxabs = m;                                                          \
      }                                                                        \
    }                                                                          \
  } while (0)

// Normal equations: target is the planar residual (row[c] - left), features the
// four neighbours above, both divided by sc (see header). sc is 1 for narrow
// samples, so that path is unchanged.
#define WP_ACCUM(T)                                                            \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    const double inv = 1.0 / sc;                                               \
    for (size_t r = 2; r < rows; ++r) {                                        \
      const T *row = s + r * w;                                                \
      const T *up = s + (r - 1) * w;                                           \
      const T *up2 = s + (r - 2) * w;                                          \
      for (size_t c = 1; c + 1 < w; ++c) {                                     \
        double f[4] = {(double)up[c] * inv, (double)up[c - 1] * inv,           \
                       (double)up[c + 1] * inv, (double)up2[c] * inv};         \
        double tgt = ((double)row[c] - (double)row[c - 1]) * inv;              \
        for (int i = 0; i < 4; ++i) {                                          \
          v[i] += f[i] * tgt;                                                  \
          for (int j = 0; j < 4; ++j)                                          \
            M[i][j] += f[i] * f[j];                                            \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

static double plane_H(const uint32_t *hist, size_t nb) {
  const double invN = 1.0 / (double)nb;
  double H = 0.0;
  for (int k = 0; k < 256; ++k) {
    uint32_t ct = hist[k];
    if (ct) {
      double p = ct * invN;
      H -= p * log2(p);
    }
  }
  return H;
}

// Two scorers, not one duplicated. WIDE reproduces the encoder exactly. NARROW
// takes the cheaper int64 subtract truncated to int16, which only holds at 8
// and 16 bits, and always scores two planes, over-counting every candidate alike.
#define WP_SCORE_NARROW(T, ZCAST)                                              \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    for (size_t r = 0; r < rows; ++r) {                                        \
      const size_t row = r * w;                                                \
      const T *ab = (r >= 1) ? s + row - w : (const T *)0;                     \
      const T *ab2 = (r >= 2) ? s + row - 2 * w : (const T *)0;                \
      for (size_t c = 0; c < w; ++c) {                                         \
        const int64_t N = ab ? ab[c] : 0;                                      \
        const int64_t NW = (ab && c > 0) ? ab[c - 1] : 0;                      \
        const int64_t NE = (ab && c + 1 < w) ? ab[c + 1] : 0;                  \
        const int64_t NN = ab2 ? ab2[c] : 0;                                   \
        const int64_t base =                                                   \
            (int64_t)s[row + c] - (int64_t)((c > 0) ? s[row + c - 1] : 0);     \
        for (int m = 0; m < nc; ++m) {                                         \
          const int64_t K = (cf[m][0] * N + cf[m][1] * NW + cf[m][2] * NE +    \
                             cf[m][3] * NN + rnd[m]) >>                        \
                            sh[m];                                             \
          const int16_t z = (int16_t)(ZCAST)(base - K);                        \
          const uint16_t zz = (uint16_t)(((uint16_t)z << 1) ^ (z >> 15));      \
          hist[m][1][(zz >> 8) & 0xFF]++;                                      \
          hist[m][0][zz & 0xFF]++;                                             \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

// 32/64 bit scoring: residual formed exactly as the encoder (unsigned K, signed
// shift, native-width subtract), then zigzagged and split into NB byte planes.
#define WP_SCORE_WIDE(T, NB, BITS)                                             \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    for (size_t r = 0; r < rows; ++r) {                                        \
      const size_t row = r * w;                                                \
      const T *ab = (r >= 1) ? s + row - w : (const T *)0;                     \
      const T *ab2 = (r >= 2) ? s + row - 2 * w : (const T *)0;                \
      for (size_t c = 0; c < w; ++c) {                                         \
        const uint64_t N = ab ? ab[c] : 0;                                     \
        const uint64_t NW = (ab && c > 0) ? ab[c - 1] : 0;                     \
        const uint64_t NE = (ab && c + 1 < w) ? ab[c + 1] : 0;                 \
        const uint64_t NN = ab2 ? ab2[c] : 0;                                  \
        const uint64_t Wv = (c > 0) ? s[row + c - 1] : 0;                      \
        for (int m = 0; m < nc; ++m) {                                         \
          const uint64_t cN = (uint64_t)cf[m][0], cNW = (uint64_t)cf[m][1],    \
                         cNE = (uint64_t)cf[m][2], cNN = (uint64_t)cf[m][3];   \
          const uint64_t acc =                                                 \
              cN * N + cNW * NW + cNE * NE + cNN * NN + (uint64_t)rnd[m];      \
          const int64_t K = (int64_t)acc >> sh[m];                             \
          const T res = (T)(s[row + c] - (Wv + (uint64_t)K));                  \
          const T sgn = (T)(res >> (BITS - 1));                                \
          const T zz = (T)(((T)(res << 1)) ^ (T)((T)0 - sgn));                 \
          for (int b = 0; b < NB; ++b)                                         \
            hist[m][b][(int)((zz >> (8 * b)) & 0xFF)]++;                       \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

int wp_static_train(int16_t coeffs[4], uint8_t *shift, const void *src,
                    size_t width, size_t nb_elts, size_t elt_width) {
  coeffs[0] = 1;
  coeffs[1] = -1;
  coeffs[2] = 0;
  coeffs[3] = 0;
  *shift = 0;

  if (elt_width != 1 && elt_width != 2 && elt_width != 4 && elt_width != 8)
    return 1;
  // covers nb_elts == 0 and a width that does not divide it
  const size_t w = geozl_row_width(width, nb_elts);
  if (w == 0)
    return 1;
  // too small to fit a predictor, the defaults above stand
  const size_t rows = nb_elts / w;
  if (rows < 3 || w < 3)
    return 0;

  double sc = 1.0;
  if (elt_width >= 4) {
    double maxabs = 1.0;
    if (elt_width == 4)
      WP_MAXABS(uint32_t);
    else
      WP_MAXABS(uint64_t);
    sc = maxabs;
  }

  double M[4][4] = {{0}}, v[4] = {0};
  switch (elt_width) {
  case 1:
    WP_ACCUM(uint8_t);
    break;
  case 2:
    WP_ACCUM(uint16_t);
    break;
  case 4:
    WP_ACCUM(uint32_t);
    break;
  default:
    WP_ACCUM(uint64_t);
    break;
  }

  double tr = M[0][0] + M[1][1] + M[2][2] + M[3][3];
  double lambda =
      1e-6 * (tr / 4.0) + 1e-9; // ridge, the neighbours are collinear
  for (int i = 0; i < 4; ++i)
    M[i][i] += lambda;

  double a[4];
  if (!solve4(M, v, a))
    return 0; // singular, the defaults set at the top stand

  int16_t cf[WP_MAX_CAND][4];
  uint8_t sh[WP_MAX_CAND];
  int32_t rnd[WP_MAX_CAND];
  int nc = 0;
  cf[0][0] = 1;
  cf[0][1] = -1;
  cf[0][2] = 0;
  cf[0][3] = 0; // planar
  sh[0] = 0;
  rnd[0] = 0;
  nc = 1;
  for (int s = 0; s <= WP_SHIFT_MAX; ++s) {
    int16_t q[4];
    int ok = 1;
    for (int k = 0; k < 4; ++k) {
      long qi = lround(a[k] * (double)(1 << s));
      if (qi < -32768 || qi > 32767) {
        ok = 0;
        break;
      }
      q[k] = (int16_t)qi;
    }
    if (!ok)
      continue;
    cf[nc][0] = q[0];
    cf[nc][1] = q[1];
    cf[nc][2] = q[2];
    cf[nc][3] = q[3];
    sh[nc] = (uint8_t)s;
    rnd[nc] = s ? (int32_t)1 << (s - 1) : 0;
    ++nc;
  }

  // One 256-bin plane per sample byte: planes 0-1 for narrow, eltWidth for
  // wide. Heap rather than a static buffer, which two threads compressing at
  // once would share, and too large to sit on the stack of a worker thread.
  // Failing to allocate is not an error, the planar defaults set at the top
  // stand and the tile just does not get a fitted predictor.
  uint32_t (*hist)[WP_MAX_PLANES][256] = calloc(WP_MAX_CAND, sizeof *hist);
  if (hist == NULL)
    return 0;
  int nplanes;
  switch (elt_width) {
  case 1:
    WP_SCORE_NARROW(uint8_t, int8_t);
    nplanes = 2;
    break;
  case 2:
    WP_SCORE_NARROW(uint16_t, int16_t);
    nplanes = 2;
    break;
  case 4:
    WP_SCORE_WIDE(uint32_t, 4, 32);
    nplanes = 4;
    break;
  default:
    WP_SCORE_WIDE(uint64_t, 8, 64);
    nplanes = 8;
    break;
  }

  // Cost of every candidate under the byte plane proxy. Candidate 0 is planar.
  double planar_cost = 0.0;
  for (int p = 0; p < nplanes; ++p)
    planar_cost += plane_H(hist[0][p], nb_elts);

  // The order-0 byte-plane proxy is close to the coded size at 1/2 bytes, so
  // the choice is a plain argmin there (margin 0, unchanged). At 4/8 bytes the
  // proxy is weaker, so a candidate must beat planar by a small relative margin
  // to be taken, which stops a tile from ever coming out larger than planar.
  const double margin = (elt_width >= 4) ? planar_cost * 0.003 : 0.0;

  int best = 0;
  double best_cost = planar_cost;
  for (int m = 1; m < nc; ++m) {
    double e = 0.0;
    for (int p = 0; p < nplanes; ++p)
      e += plane_H(hist[m][p], nb_elts);
    if (e < planar_cost - margin && e < best_cost) {
      best_cost = e;
      best = m;
    }
  }

  coeffs[0] = cf[best][0];
  coeffs[1] = cf[best][1];
  coeffs[2] = cf[best][2];
  coeffs[3] = cf[best][3];
  *shift = sh[best];
  free(hist);
  return 0;
}
