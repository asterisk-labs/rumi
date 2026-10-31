#ifndef GEOZL_EXPORT_H
#define GEOZL_EXPORT_H

// Public symbols keep default visibility under -fvisibility=hidden so cffi can
// find the kernels and a linker can resolve the register entry points.
#if defined(GEOZL_STATIC)
#define GEOZL_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(GEOZL_BUILD)
#define GEOZL_API __declspec(dllexport)
#else
#define GEOZL_API __declspec(dllimport)
#endif
#else
#define GEOZL_API __attribute__((visibility("default")))
#endif

#endif
