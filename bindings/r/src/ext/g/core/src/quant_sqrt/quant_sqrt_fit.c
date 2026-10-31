#include "quant_sqrt_fit.h"

#include "quant_sqrt_dtype.h"
#include "quant_sqrt_half.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QSQ_BLK 8      // block side, in samples
#define QSQ_NBIN 24    // intensity bins
#define QSQ_MINBLK 12  // blocks a bin needs before it counts
#define QSQ_MINBIN 4   // bins the fit needs before it means anything
#define QSQ_PCT 0.10   // quantile of the local variance taken per bin
#define QSQ_CAP (1u << 18) // pairs held before blocks start being dropped

// The quantile is biased low and the bias is a constant, not noise.
//
// The Immerkaer windows inside a block overlap, so the 64 responses are not 64
// independent samples. Measured against pure noise of known sigma, the effective
// degrees of freedom of an 8x8 block are 19.3 rather than 64, and the tenth
// percentile lands at 0.7904 of the true sigma. In variance that is 0.6247.
//
// The number belongs to QSQ_BLK and to the mask below and to nothing else.
// Changing either without measuring this again moves every bound the codec
// declares, and moves it quietly, since the fit will still look reasonable.
#define QSQ_PCT_BIAS 0.624732

static int fail(char *err, size_t errSize, const char *fmt, ...) {
  if (err != NULL && errSize != 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errSize, fmt, ap);
    va_end(ap);
  }
  return 1;
}

static int cmpd(const void *p, const void *q) {
  const double a = *(const double *)p, b = *(const double *)q;
  return a < b ? -1 : (a > b ? 1 : 0);
}

void quant_sqrt_accum_init(quant_sqrt_accum *acc) {
  memset(acc, 0, sizeof(*acc));
  acc->stride = 1;
}

void quant_sqrt_accum_free(quant_sqrt_accum *acc) {
  free(acc->mu);
  free(acc->s2);
  quant_sqrt_accum_init(acc);
}

// Beyond the cap the buffer keeps every other pair it already holds and doubles
// the stride, so memory is bounded and what survives is still spread evenly over
// everything that was pushed.
static void decimate(quant_sqrt_accum *acc) {
  size_t k = 0;
  for (size_t i = 0; i < acc->n; i += 2) {
    acc->mu[k] = acc->mu[i];
    acc->s2[k] = acc->s2[i];
    ++k;
  }
  acc->n = k;
  acc->stride *= 2;
}

static int push_pair(quant_sqrt_accum *acc, double mu, double s2) {
  if (acc->seen++ % acc->stride != 0)
    return 0;
  if (acc->n == acc->cap) {
    if (acc->cap >= QSQ_CAP) {
      decimate(acc);
    } else {
      const size_t cap = acc->cap ? acc->cap * 2 : 4096;
      double *m = (double *)realloc(acc->mu, cap * sizeof(double));
      if (m == NULL) { acc->failed = 1; return 1; }
      acc->mu = m;
      double *v = (double *)realloc(acc->s2, cap * sizeof(double));
      if (v == NULL) { acc->failed = 1; return 1; }
      acc->s2 = v;
      acc->cap = cap;
    }
  }
  acc->mu[acc->n] = mu;
  acc->s2[acc->n] = s2;
  ++acc->n;
  return 0;
}

// A band of QSQ_BLK + 2 rows widened to double, so the block loop below is plain
// arithmetic with no per-sample switch and the scratch stays bounded whatever the
// raster width is.
#define QSQ_WIDEN(RD)                                                          \
  do {                                                                         \
    for (size_t r = 0; r < rows; ++r) {                                        \
      const size_t y = y0 + r;                                                 \
      for (size_t x = 0; x < width; ++x) {                                     \
        const size_t i = y * width + x;                                        \
        band[r * width + x] = (double)(RD);                                    \
      }                                                                        \
    }                                                                          \
  } while (0)

static void widen(double *band, const void *src, int dtype, size_t width,
                  size_t y0, size_t rows) {
  switch ((qsq_dtype)dtype) {
  case QSQ_U8: QSQ_WIDEN(((const uint8_t *)src)[i]); break;
  case QSQ_U16: QSQ_WIDEN(((const uint16_t *)src)[i]); break;
  case QSQ_U32: QSQ_WIDEN(((const uint32_t *)src)[i]); break;
  case QSQ_U64: QSQ_WIDEN(((const uint64_t *)src)[i]); break;
  case QSQ_I8: QSQ_WIDEN(((const int8_t *)src)[i]); break;
  case QSQ_I16: QSQ_WIDEN(((const int16_t *)src)[i]); break;
  case QSQ_I32: QSQ_WIDEN(((const int32_t *)src)[i]); break;
  case QSQ_I64: QSQ_WIDEN(((const int64_t *)src)[i]); break;
  case QSQ_F16:
    QSQ_WIDEN(quant_sqrt_half_to_float(((const uint16_t *)src)[i]));
    break;
  case QSQ_F32: QSQ_WIDEN(((const float *)src)[i]); break;
  default: QSQ_WIDEN(((const double *)src)[i]); break;
  }
}

int quant_sqrt_accum_push(quant_sqrt_accum *acc, const void *src, int dtype,
                          size_t width, size_t height) {
  if (acc == NULL || src == NULL || !QSQ_DTYPE_OK(dtype))
    return 1;
  if (width < QSQ_BLK + 2 || height < QSQ_BLK + 2)
    return 1;

  const size_t bw = (width - 2) / QSQ_BLK, bh = (height - 2) / QSQ_BLK;
  const size_t rows = QSQ_BLK + 2;
  double *band = (double *)malloc(rows * width * sizeof(double));
  if (band == NULL) { acc->failed = 1; return 1; }

  for (size_t by = 0; by < bh; ++by) {
    widen(band, src, dtype, width, by * QSQ_BLK, rows);
    for (size_t bx = 0; bx < bw; ++bx) {
      double sum = 0.0, acc2 = 0.0;
      int bad = 0;
      for (size_t j = 0; j < QSQ_BLK; ++j) {
        const double *r0 = band + j * width;
        const double *r1 = r0 + width;
        const double *r2 = r1 + width;
        for (size_t i = 0; i < QSQ_BLK; ++i) {
          const size_t x = 1 + bx * QSQ_BLK + i;
          // Immerkaer. The mask is [1 -2 1; -2 4 -2; 1 -2 1], whose response to
          // any linear ramp is zero, which is what keeps a smooth gradient from
          // reading as noise. sum(M^2) is 36.
          const double r = 4.0 * r1[x]
                           - 2.0 * (r0[x] + r2[x] + r1[x - 1] + r1[x + 1])
                           + (r0[x - 1] + r0[x + 1] + r2[x - 1] + r2[x + 1]);
          if (!isfinite(r) || !isfinite(r1[x])) { bad = 1; break; }
          sum += r1[x];
          acc2 += r * r;
        }
        if (bad)
          break;
      }
      if (bad)
        continue; // a block with a non-finite sample says nothing about the noise
      const double n = (double)(QSQ_BLK * QSQ_BLK);
      if (push_pair(acc, sum / n, acc2 / (36.0 * n) / QSQ_PCT_BIAS) != 0) {
        free(band);
        return 1;
      }
    }
  }
  free(band);
  return 0;
}

int quant_sqrt_accum_solve(const quant_sqrt_accum *acc, quant_sqrt_noise *out,
                           char *err, size_t errSize) {
  memset(out, 0, sizeof(*out));
  if (acc == NULL || out == NULL)
    return fail(err, errSize, "no accumulator");
  if (acc->failed)
    return fail(err, errSize, "the fit ran out of memory");
  out->blocks = (int)(acc->n > (size_t)INT32_MAX ? INT32_MAX : acc->n);
  if (acc->n < (size_t)(QSQ_MINBIN * QSQ_MINBLK))
    return fail(err, errSize,
                "%zu blocks is not enough to fit a curve, a raster needs to be "
                "at least a few hundred blocks across",
                acc->n);

  double lo = acc->mu[0], hi = acc->mu[0];
  for (size_t i = 1; i < acc->n; ++i) {
    if (acc->mu[i] < lo) lo = acc->mu[i];
    if (acc->mu[i] > hi) hi = acc->mu[i];
  }
  if (!(hi > lo))
    return fail(err, errSize, "the raster has no dynamic range to fit against");

  double *buf = (double *)malloc(acc->n * sizeof(double));
  if (buf == NULL)
    return fail(err, errSize, "the fit ran out of memory");

  double bmu[QSQ_NBIN], bs2[QSQ_NBIN], bw[QSQ_NBIN];
  int nk = 0;
  for (int t = 0; t < QSQ_NBIN; ++t) {
    const double e0 = lo + (hi - lo) * t / QSQ_NBIN;
    const double e1 = lo + (hi - lo) * (t + 1) / QSQ_NBIN;
    size_t c = 0;
    double msum = 0.0;
    for (size_t i = 0; i < acc->n; ++i) {
      const double m = acc->mu[i];
      if (m >= e0 && (m < e1 || (t == QSQ_NBIN - 1 && m <= e1))) {
        buf[c++] = acc->s2[i];
        msum += m;
      }
    }
    if (c < (size_t)QSQ_MINBLK)
      continue;
    // A low quantile, not the mean. The mean is unbiased on pure noise and
    // useless on a real scene, because every textured block in the bin lifts it
    // and there is no way to tell texture from noise after the fact.
    qsort(buf, c, sizeof(double), cmpd);
    size_t idx = (size_t)(QSQ_PCT * (double)c);
    if (idx >= c)
      idx = c - 1;
    bmu[nk] = msum / (double)c;
    bs2[nk] = buf[idx];
    bw[nk] = (double)c;
    ++nk;
  }
  free(buf);
  out->bins = nk;
  if (nk < QSQ_MINBIN)
    return fail(err, errSize,
                "only %d intensity bins held enough blocks, the raster does not "
                "cover enough of its range",
                nk);

  double Sw = 0, Sx = 0, Sy = 0, Sxx = 0, Sxy = 0;
  for (int k = 0; k < nk; ++k) {
    const double w = bw[k];
    Sw += w;
    Sx += w * bmu[k];
    Sy += w * bs2[k];
    Sxx += w * bmu[k] * bmu[k];
    Sxy += w * bmu[k] * bs2[k];
  }
  const double det = Sw * Sxx - Sx * Sx;
  if (!(fabs(det) > 0.0))
    return fail(err, errSize, "the intensity bins are degenerate");
  out->b = (Sw * Sxy - Sx * Sy) / det;
  out->a = (Sy - out->b * Sx) / Sw;
  // A negative intercept is a variance at zero signal, which does not exist. It
  // comes from extrapolating past where the data reaches, so it is floored rather
  // than trusted.
  if (out->a < 0.0)
    out->a = 0.0;

  double rs = 0.0, ys = 0.0;
  for (int k = 0; k < nk; ++k) {
    const double p = out->a + out->b * bmu[k];
    rs += (bs2[k] - p) * (bs2[k] - p);
    ys += bs2[k] * bs2[k];
  }
  out->resid = sqrt(rs / (ys > 0.0 ? ys : 1.0));

  // What decides whether a and b are separable is not the span, which always
  // fills its own range, but the ratio between the ends and how far the means
  // spread around their centre. A narrow band gives a plausible curve that means
  // nothing, and only these two numbers say so.
  out->range = bmu[0] > 0.0 ? bmu[nk - 1] / bmu[0] : INFINITY;
  {
    const double var = Sxx / Sw - (Sx / Sw) * (Sx / Sw);
    out->colin = var > 0.0 ? fabs(Sx / Sw) / sqrt(var) : INFINITY;
  }

  if (!(out->b > 0.0))
    return fail(err, errSize,
                "the local variance does not grow with the signal, so this is "
                "not shot noise");
  out->ok = 1;
  return 0;
}

int quant_sqrt_fit(const void *src, int dtype, size_t width, size_t height,
                   quant_sqrt_noise *out, char *err, size_t errSize) {
  quant_sqrt_accum acc;
  quant_sqrt_accum_init(&acc);
  memset(out, 0, sizeof(*out));
  if (quant_sqrt_accum_push(&acc, src, dtype, width, height) != 0) {
    quant_sqrt_accum_free(&acc);
    return fail(err, errSize,
                "a raster of %zux%zu is too small to measure blocks on", width,
                height);
  }
  const int rc = quant_sqrt_accum_solve(&acc, out, err, errSize);
  quant_sqrt_accum_free(&acc);
  return rc;
}
