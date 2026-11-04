// Floating point limits shared by the lossy codecs.

#ifndef GEOZL_COMMON_FP_H
#define GEOZL_COMMON_FP_H

// 2^53. Largest whole number a double carries with no gap, so the last index a
// grid can name and still round trip, and the point past which a value is
// already an integer.
#define GEOZL_F64_EXACT_INT 9007199254740992.0

// The largest finite double. Above it is an infinity, which the quantizers pin
// to the top level rather than running a transform on an exponent of all ones.
#define GEOZL_F64_MAX 1.7976931348623157e308

// An infinity from plain arithmetic, for code that does not include math.h.
#define GEOZL_F64_INF (GEOZL_F64_MAX * 10.0)

#endif // GEOZL_COMMON_FP_H
