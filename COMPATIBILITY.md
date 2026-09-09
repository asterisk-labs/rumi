# Compatibility

Rumi 0.18 is the current compatibility baseline. Current readers open
canonical files written by 0.18.0 and later. Earlier versions are not
supported. The format may still change before 1.0; any compatibility break
will be recorded in the changelog.

A Rumi file contains everything needed to rebuild its external header. Use
`rumi.info(source=...).header` in Python, or `rumi_info` in C, when the header
returned by the writer is unavailable.

## Writers and readers

Compatibility guarantees apply to files created by Rumi's writer. The writer
is available as `rumi.write` in Python and `rumi_write` in C. The reader may
accept files from independent writers, but they are outside this policy.

Independent readers are supported. [SPEC.md](SPEC.md) contains everything
needed to read a `.rumi` file without this library. If a file written by Rumi
cannot be read from the specification, that is a specification bug.

## APIs

The public C source API and binary ABI are not stable before 1.0, so C
applications should be recompiled after an update. `RUMI_API_VERSION` and the
shared-library SONAME remain at 1 throughout this unstable period. Starting
with Rumi 1.0, incompatible ABI changes will increment them.

The Python API follows semantic versioning. Before 1.0, a minor release may
contain a breaking change; it will be called out in the changelog.

## GeoZL and OpenZL

Rumi 0.19.x uses GeoZL 0.16.x as its frame compatibility baseline and includes
OpenZL 0.2.0. The Python `write` extra requires GeoZL 0.16.x.
