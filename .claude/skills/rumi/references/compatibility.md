# Compatibility

Sources: `COMPATIBILITY.md`, `CHANGELOG.md`, `SPEC.md`, `NOTICE`, `.gitmodules`,
`bindings/python/pyproject.toml`, `tools/check_release.py`.

## Contents

1. The policy
2. Pinned dependencies
3. Deploying readers and writers
4. Changing the format or an API

## 1. The policy

- **Files.** Valid sample encodings are listed in `SPEC.md`; every other pair is
  rejected. Reserved encoding numbers are never assigned another meaning. Earlier
  files are unsupported. The format may still change before 1.0, and any break is
  recorded in `CHANGELOG.md`.
- **Writers.** The guarantee covers files written by `rumi.write` and `rumi_write`. The
  reader may accept files from other writers, but they are outside the policy.
- **Independent readers** are supported: `SPEC.md` must be enough to read any file Rumi
  writes. A Rumi file that cannot be read from the specification is a specification bug.
- **Headers are derived.** Every file rebuilds its header (`rumi.info(source=...).header`
  or `rumi_info`), so a lost or stale header is recoverable.
- **C.** No stable source API or ABI before 1.0. The development API and SONAME remain
  at 1 even when layouts change; recompile C applications after every update. Starting
  with Rumi 1.0, incompatible ABI changes increment them. Python checks the dtype-row
  size before reading the registry.
- **Python** follows semantic versioning, but before 1.0 a minor release may break the
  API; the changelog calls it out.

## 2. Pinned dependencies

| Component | Rumi 0.29.0 | Where it is pinned |
| --- | --- | --- |
| GeoZL | 0.31.0 | `extern/geozl` submodule; `geozl>=0.31.0,<0.32` in the Python dependencies; `@asterisk-labs/geozl` 0.31.0 in the JavaScript package |
| OpenZL | upstream commit `b7f498b` | through GeoZL's submodule |
| Karu | 0.4.0 | `extern/karu` submodule; named in `NOTICE` |
| curl, OpenSSL | 8.22.0, 3.x | bundled in release wheels (`tools/build_static_curl.sh` on Linux) |

- `tools/check_release.py` fails when the dependencies do not read
  `geozl>=X.Y.Z,<X.(Y+1)` for the submodule's `VERSION`, or when `NOTICE` names another
  Karu version.

## 3. Deploying readers and writers

- Upgrade every reader before any writer. A frame from a GeoZL newer than the reader's
  fails with `NotImplementedError: rumi: file uses a geozl codec this build lacks, update
  geozl (CTid N)`.
- Record `rumi.__version__` and `geozl.__version__` with the dataset.
- Do not use file or frame hashes as content identity across GeoZL versions: encoders
  may produce different bytes for the same samples. Compare decoded data.
- Headers can always be rebuilt from their files with `rumi.info(source=...)`. Files
  written before a format break are not readable afterwards and must be rewritten.

## 4. Changing the format or an API

A format change (file header, IFD, header blob, trailer, a registry):

- `SPEC.md` and its figures in `img/`; the specification is normative.
- `core/src/parser.cpp`, `builder.cpp`, `write.cpp`, `trailer.cpp`, `geokeys.cpp` as
  needed.
- The independent parsers and golden digests: `bindings/python/tests/test_spec.py`,
  `test_writer.py` (`GOLDEN`, "update only for an intentional format change"),
  `test_independent_reader.py`, and `core/tests/test_core.cpp`.
- Fuzz seeds: `python tools/make_fuzz_seeds.py` regenerates `fuzz/replay/`.
- `CHANGELOG.md` under `### Breaking`, `COMPATIBILITY.md` when the baseline moves, and
  `references/format.md` in this skill.
- Dtype codes and file encodings keep their meaning. Reserved values are never reused.
  `frame_unit` values stay append-only.

A C API change: `rumi.h`, `capi.cpp`, the cdef in `_ffi.py`, and the `_PUBLIC_API`
signatures recorded in `test_cdef.py` ("update this list only for an intentional API
change"); keep `test_c_header.c` compiling.

A Python API change: `README.md` (its Python blocks run in `test_readme.py`),
`docs/examples.html`, `docs/how-rumi-reads.html`, `examples/rumi-vs-geotiff.ipynb`
and `references/python-api.md` in this skill.
