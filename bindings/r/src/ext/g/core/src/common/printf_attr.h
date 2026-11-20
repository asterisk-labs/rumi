// Marks a printf-like helper, so its callers' arguments are checked against the
// format and the vsnprintf inside it does not count as a nonliteral format.

#ifndef GEOZL_COMMON_PRINTF_ATTR_H
#define GEOZL_COMMON_PRINTF_ATTR_H

// MinGW gcc reads printf as the msvcrt grammar, which has no %zu; R's headers
// take gnu_printf there for the same reason.
#if defined(__MINGW32__) && !defined(__clang__)
#define GEOZL_PRINTF(fmt, first) __attribute__((format(gnu_printf, fmt, first)))
#elif defined(__GNUC__) || defined(__clang__)
#define GEOZL_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define GEOZL_PRINTF(fmt, first)
#endif

#endif // GEOZL_COMMON_PRINTF_ATTR_H
