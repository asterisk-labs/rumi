// log2 and exp2, written out instead of taken from libm.
//
// A frame is written on one machine and read on another, and libm is not
// correctly rounded, so two platforms can disagree by an ulp and rebuild the
// same index two ways. Everything here is plain IEEE arithmetic and with
// contraction off it evaluates the same anywhere.
//
// Both work in log2. The integer part of log2 of a float is its exponent field,
// so it is exact and only the mantissa needs a series.

#ifndef GEOZL_CODECS_QUANT_LOG_MATH_H
#define GEOZL_CODECS_QUANT_LOG_MATH_H

#include "common/fp.h"

#include <stdint.h>
#include <string.h>

#define QLOG_LN2 0.69314718055994530942
#define QLOG_LOG2E 1.44269504088896340736

// Round half away from zero. nearbyint follows the current rounding mode, which
// the decoder cannot depend on.
//
// The conversion is undefined past what an int64 holds, and lands somewhere
// different on x86 and on arm64, so anything that large is returned as it
// stands; it is already whole. A nan fails the comparison and takes the same
// exit, which keeps this total.
static inline double qlog_round(double v) {
  if (!(v > -GEOZL_F64_EXACT_INT && v < GEOZL_F64_EXACT_INT))
    return v;
  return v < 0.0 ? -(double)(int64_t)(0.5 - v) : (double)(int64_t)(v + 0.5);
}

// 1 KB per translation unit that includes this. Kept here on purpose: as
// externs in a .c they would be one copy, but every caller that links the
// kernels by hand would then have a new file to add, which is a worse trade
// than the kilobyte.
static const double qlog_mlg[64] = {
    1.12272554232541195e-02, 3.34230015374502795e-02, 5.52824355011896015e-02, 7.68155970508308944e-02,
    9.80320829605267202e-02, 1.18941072723507429e-01, 1.39551352398793543e-01, 1.59871336778389411e-01,
    1.79909090014934464e-01, 1.99672344836364396e-01, 2.19168520462161565e-01, 2.38404739325078913e-01,
    2.57387842692651747e-01, 2.76124405274237539e-01, 2.94620748891626982e-01, 3.12882955284355335e-01,
    3.30916878114616952e-01, 3.48728154231077558e-01, 3.66322214245815792e-01, 3.83704292474052244e-01,
    4.00879436282184309e-01, 4.17852514885897863e-01, 4.34628227636724651e-01, 4.51211111832328815e-01,
    4.67605550082997423e-01, 4.83815777264256397e-01, 4.99845887083205376e-01, 5.15699838284042422e-01,
    5.31381460516312076e-01, 5.46894459887636630e-01, 5.62242424221072623e-01, 5.77428828035748687e-01,
    5.92457037268080411e-01, 6.07330313749610662e-01, 6.22051819456376220e-01, 6.36624620543648878e-01,
    6.51051691178928582e-01, 6.65335917185176262e-01, 6.79480099505446078e-01, 6.93486957499325207e-01,
    7.07359132080882747e-01, 7.21099188707185146e-01, 7.34709620225838189e-01, 7.48192849589460307e-01,
    7.61551232444479309e-01, 7.74787059601173445e-01, 7.87902559391431612e-01, 8.00899899920304748e-01,
    8.13781191217037070e-01, 8.26548487290915013e-01, 8.39203788096943959e-01, 8.51749041416057562e-01,
    8.64186144654280231e-01, 8.76516946564999677e-01, 8.88743248898259064e-01, 9.00866807980748585e-01,
    9.12889336229961601e-01, 9.24812503605780933e-01, 9.36637939002570530e-01, 9.48367231584677617e-01,
    9.60001932068080932e-01, 9.71543553950771965e-01, 9.82993574694310146e-01, 9.94353436858857909e-01,
};
static const double qlog_mrc[64] = {
    9.92248062015503862e-01, 9.77099236641221336e-01, 9.62406015037593932e-01, 9.48148148148148184e-01,
    9.34306569343065663e-01, 9.20863309352518034e-01, 9.07801418439716290e-01, 8.95104895104895104e-01,
    8.82758620689655160e-01, 8.70748299319727859e-01, 8.59060402684563740e-01, 8.47682119205298013e-01,
    8.36601307189542509e-01, 8.25806451612903225e-01, 8.15286624203821697e-01, 8.05031446540880546e-01,
    7.95031055900621064e-01, 7.85276073619631920e-01, 7.75757575757575757e-01, 7.66467065868263520e-01,
    7.57396449704141994e-01, 7.48538011695906391e-01, 7.39884393063583778e-01, 7.31428571428571428e-01,
    7.23163841807909602e-01, 7.15083798882681587e-01, 7.07182320441988921e-01, 6.99453551912568305e-01,
    6.91891891891891930e-01, 6.84491978609625629e-01, 6.77248677248677211e-01, 6.70157068062827266e-01,
    6.63212435233160646e-01, 6.56410256410256410e-01, 6.49746192893400965e-01, 6.43216080402010060e-01,
    6.36815920398009938e-01, 6.30541871921182273e-01, 6.24390243902439024e-01, 6.18357487922705285e-01,
    6.12440191387559785e-01, 6.06635071090047440e-01, 6.00938967136150248e-01, 5.95348837209302317e-01,
    5.89861751152073732e-01, 5.84474885844748826e-01, 5.79185520361991002e-01, 5.73991031390134521e-01,
    5.68888888888888888e-01, 5.63876651982378907e-01, 5.58951965065502154e-01, 5.54112554112554112e-01,
    5.49356223175965663e-01, 5.44680851063829796e-01, 5.40084388185653963e-01, 5.35564853556485310e-01,
    5.31120331950207469e-01, 5.26748971193415683e-01, 5.22448979591836782e-01, 5.18218623481781382e-01,
    5.14056224899598346e-01, 5.09960159362549792e-01, 5.05928853754940677e-01, 5.01960784313725483e-01,
};

// log2(x) - anchorExp2, for finite x > 0. Folding the anchor in here cancels it
// against the exponent field before anything is added, so nothing large is ever
// subtracted from anything large.
static inline double qlog_log2_over(double x, int anchorExp2) {
  uint64_t b;
  memcpy(&b, &x, sizeof(b));
  int e = (int)((b >> 52) & 0x7FFu);

  // A subnormal has no implicit leading one. 2^54 is exact and cannot overflow
  // from down there.
  int sub = 0;
  if (e == 0) {
    x *= 18014398509481984.0;
    memcpy(&b, &x, sizeof(b));
    e = (int)((b >> 52) & 0x7FFu);
    sub = 54;
  }

  const uint64_t man = b & 0x000FFFFFFFFFFFFFull;
  const unsigned k = (unsigned)(man >> 46);
  uint64_t mb = man | 0x3FF0000000000000ull;
  double m;
  memcpy(&m, &mb, sizeof(m));

  // The table point is a reciprocal, so this is a multiply and not a divide.
  // |w| stays under 1/128 and the tail past the sixth power is under 4e-16.
  const double w = m * qlog_mrc[k] - 1.0;
  const double w2 = w * w;
  const double w4 = w2 * w2;
  const double a = 1.0 - 0.5 * w;
  const double c = 0.33333333333333333333 - 0.25 * w;
  const double d = 0.2 - 0.16666666666666666667 * w;

  return (double)(e - 1023 - sub - anchorExp2) + qlog_mlg[k] +
         (w * ((a + w2 * c) + w4 * d)) * QLOG_LOG2E;
}

// 2^f for |f| <= 0.5, as exp(f*ln2). The tail past the thirteenth term is under
// 1e-17.
static inline double qlog_exp2_frac(double f) {
  const double g = f * QLOG_LN2;
  double p = 1.6059043836821614599e-10; // 1/13!
  p = p * g + 2.0876756987868098979e-9;
  p = p * g + 2.5052108385441718775e-8;
  p = p * g + 2.7557319223985890653e-7;
  p = p * g + 2.7557319223985890653e-6;
  p = p * g + 2.4801587301587301587e-5;
  p = p * g + 1.9841269841269841270e-4;
  p = p * g + 1.3888888888888888889e-3;
  p = p * g + 8.3333333333333333333e-3;
  p = p * g + 4.1666666666666666667e-2;
  p = p * g + 1.6666666666666666667e-1;
  p = p * g + 0.5;
  p = p * g + 1.0;
  return p * g + 1.0;
}

// 2^y. Splitting off the integer part leaves the series a bounded argument and
// turns the rest into an exponent, which is exact. Out of range saturates.
static inline double qlog_exp2(double y) {
  // Negated so a nan takes the floor rather than reaching the cast below, where
  // it would be undefined. params_ok rules out the step that could produce one,
  // but this is a header and the next caller may not have gone through it.
  if (!(y > -1200.0))
    return 0.0;
  if (y >= 1024.0)
    return GEOZL_F64_INF;
  const double n = qlog_round(y);
  double v = qlog_exp2_frac(y - n);
  int k = (int)n;

  // Scaling by hand, in steps, because the exponent alone can leave the normal
  // range on the way even when the product does not.
  while (k > 1023) {
    v *= 8.98846567431158e307;
    k -= 1023;
  }
  while (k < -1022) {
    v *= 2.2250738585072014e-308;
    k += 1022;
  }
  uint64_t sb = ((uint64_t)(k + 1023)) << 52;
  double scale;
  memcpy(&scale, &sb, sizeof(scale));
  return v * scale;
}

// A level of the index grid, for index magnitude m >= 1.
static inline double qlog_index_level(double m, double step, int anchorExp2) {
  return qlog_exp2((double)anchorExp2 + (m - 1.0) * step);
}

// A level of the value grid, anchored at one, rounded to a whole number. Past
// 2^53 a double holds nothing else, so the rounding is already done.
static inline double qlog_value_level(double j, double step) {
  const double v = qlog_exp2(j * step);
  return v >= GEOZL_F64_EXACT_INT ? v : qlog_round(v);
}

#endif // GEOZL_CODECS_QUANT_LOG_MATH_H
