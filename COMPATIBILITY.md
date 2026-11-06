# Compatibility

Rumi 0.25 is the released compatibility baseline. Valid sample encodings are
listed in `SPEC.md`; every other pair is rejected. Reserved encoding numbers
are never assigned another meaning. Earlier versions are not supported. The
format may still change before 1.0; any compatibility break is recorded in the
changelog.

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
shared-library SONAME remain at 1 throughout this unstable period, even when
layouts change. C applications must be recompiled after an update. The Python
binding verifies the dtype-registry row size before reading it. Starting with
Rumi 1.0, incompatible ABI changes will increment the API version and SONAME.

The Python API follows semantic versioning. Before 1.0, a minor release may
contain a breaking change; it will be called out in the changelog.

## GeoZL and OpenZL

Rumi 0.27.0 includes GeoZL 0.20.0 and OpenZL 0.3.0. Its Python package requires
`geozl>=0.20.0,<0.21`. GeoZL 0.20 reads the older
frames used by Rumi, while byte-lane PivCo frames require a GeoZL 0.20 reader.

Rumi 0.28.0 includes GeoZL 0.21.0 and requires `geozl>=0.21.0,<0.22` in
Python. Its GPU reads decode multi-chunk OpenZL frames on the CPU before
placing their samples on the GPU. The Rumi file format is unchanged.

Rumi 0.28.1 includes GeoZL 0.21.1 and requires `geozl>=0.21.1,<0.22` in
Python. The Rumi file format is unchanged.

Rumi 0.29.0 includes GeoZL 0.31.0 and requires `geozl>=0.31.0,<0.32` in
Python. The JavaScript reader uses the same GeoZL release. GeoZL carries
upstream OpenZL commit `b7f498b`. The Rumi file format is unchanged.
