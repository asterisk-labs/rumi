# Compatibility

Rumi is pre-1.0. Each release reads and writes only the current format in
`SPEC.md`, and carries no code for older formats. A format change is recorded in
the changelog, and files written before it must be rewritten.

Valid sample encodings are listed in `SPEC.md`, and every other pair is
rejected. Reserved encoding numbers are never assigned another meaning.

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

The public C source API and binary ABI are not stable before 1.0.
`RUMI_API_VERSION` and the shared-library SONAME remain at 1 throughout this
period, so C applications must be recompiled after an update. Starting with
Rumi 1.0, incompatible ABI changes increment them. The Python and Julia
bindings require version 1, and Python also checks the dtype-registry row size.

The Python API follows semantic versioning. Before 1.0, a minor release may
contain a breaking change, and the changelog calls it out.

## GeoZL and OpenZL

The `extern/geozl` submodule is pinned to GeoZL 0.32.0. GeoZL carries upstream
OpenZL commit `b7f498b`.

The Python package requires `geozl>=0.32.0,<0.33`, the range that
`tools/check_release.py` derives from the submodule's `VERSION`. The JavaScript
reader uses `@asterisk-labs/geozl` 0.32.0.

Rumi's tests write frames with the GeoZL Python binding built from the
submodule, so they always match the C core Rumi vendors.
