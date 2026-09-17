# Compatibility and version history

Sources: `COMPATIBILITY.md`, `CHANGELOG.md`, `SPEC.md`, `NOTICE`, `.gitmodules`,
`bindings/python/pyproject.toml`, `tools/check_release.py`.

## Contents

1. The policy
2. Pinned dependencies
3. Deploying readers and writers
4. Breaking changes since the file baseline
5. Changing the format or an API

## 1. The policy

- **Files.** Current readers open canonical files written by Rumi 0.18.0 and later.
  Earlier files are unsupported. The format may still change before 1.0, and any break is
  recorded in `CHANGELOG.md`.
- **Writers.** The guarantee covers files written by `rumi.write` and `rumi_write`. The
  reader may accept files from other writers, but they are outside the policy.
- **Independent readers** are supported: `SPEC.md` must be enough to read any file Rumi
  writes. A Rumi file that cannot be read from the specification is a specification bug.
- **Headers are derived.** Every file rebuilds its header (`rumi.info(source=...).header`
  or `rumi_info`), so a lost or stale header is recoverable.
- **C.** No stable source API or ABI before 1.0. `RUMI_API_VERSION` and the SONAME stay
  at 1 until then; recompile C applications after every update.
- **Python** follows semantic versioning, but before 1.0 a minor release may break the
  API; the changelog calls it out.

## 2. Pinned dependencies

| Component | Rumi 0.21.3 | Where it is pinned |
| --- | --- | --- |
| GeoZL | 0.16.0 | `extern/geozl` submodule; `geozl>=0.16.0,<0.17` in the `write` and `test` extras |
| OpenZL | 0.2.0 | through GeoZL's submodule |
| Karu | 0.2.2 | `extern/karu` submodule; named in `NOTICE` |
| curl, OpenSSL | 8.22.0, 3.x | bundled in release wheels (`tools/build_static_curl.sh` on Linux) |

- `tools/check_release.py` fails when the extras do not read `geozl>=X.Y.Z,<X.(Y+1)` for
  the submodule's `VERSION`, or when `NOTICE` names another Karu version.
- `COMPATIBILITY.md` still names 0.19.x as the release that adopted GeoZL 0.16.x; later
  releases keep the same GeoZL line.
- Frames: Rumi links GeoZL 0.16, which decodes frames written by GeoZL 0.14 and later.
  GeoZL 0.16 planar recipes emit fused codecs that GeoZL 0.15 cannot decode, so Rumi
  releases before 0.19.0 cannot read them.

## 3. Deploying readers and writers

- Upgrade every reader before any writer. A frame from a GeoZL newer than the reader's
  fails with `NotImplementedError: rumi: file uses a geozl codec this build lacks, update
  geozl (CTid N)`.
- Record `rumi.__version__` and `geozl.__version__` with the dataset.
- Do not use file or frame hashes as content identity across GeoZL versions: encoders
  may produce different bytes for the same samples. Compare decoded data.
- Headers can always be rebuilt from their files with `rumi.info(source=...)`. Files
  written before a format break are not readable afterwards and must be rewritten.

## 4. Breaking changes since the file baseline

| Version | Change |
| --- | --- |
| 0.20.0 | `read` and `read_many` require a header for every source. Removed `rumi.chunks`, `RumiHeader`, `Geo`, `Time`, `read_geo` and `read_time` (use `info` and `Metadata`); the `n`, `t`, `b`, `y`, `x` read keywords (use `time`, `bands`, `window`, and source order); multi-source `read` (use `read_many`). C removed `rumi_read_stack`, `rumi_read_stack_dlpack`, `rumi_index_file`, `rumi_read_geo`, `rumi_read_time`. |
| 0.18.0 | Files start with the 16-byte `RUMI` header instead of a BigTIFF header (the current file baseline). Reads lost `num_threads`; the process-wide pool replaced it. C layout and read APIs gained time axes. |
| 0.17.0 | 13-tag, 276-byte IFD and 32-byte header. `rumi.frames` takes an axis pattern instead of `unit`. |

Additions since the baseline, for orientation: Cubes and `time=` (0.18.0), GeoZL 0.16
(0.19.0), `info` and `read_many` (0.20.0), `Metadata` displays (0.21.0), the Karu 0.2
transport line (0.21.0 onward).

## 5. Changing the format or an API

A format change (file header, IFD, header blob, trailer, a registry):

- `SPEC.md` and its figures in `img/`; the specification is normative.
- `core/src/parser.cpp`, `builder.cpp`, `write.cpp`, `time.cpp`, `geokeys.cpp` as needed.
- The independent parsers and golden digests: `bindings/python/tests/test_spec.py`,
  `test_writer.py` (`GOLDEN`, "update only for an intentional format change"),
  `test_independent_reader.py`, and `core/tests/test_core.cpp`.
- Fuzz seeds: `python tools/make_fuzz_seeds.py` regenerates `fuzz/replay/`.
- `CHANGELOG.md` under `### Breaking`, `COMPATIBILITY.md` when the baseline moves, and
  `references/format.md` in this skill.
- Registries are append-only: never renumber `rumi_dtypes.def` codes (`test_cdef.py`
  checks) or `frame_unit` values.

A C API change: `rumi.h`, `capi.cpp`, the cdef in `_ffi.py`, and the `_PUBLIC_API`
signatures recorded in `test_cdef.py` ("update this list only for an intentional API
change"); keep `test_c_header.c` compiling.

A Python API change: `README.md` (its Python blocks run in `test_readme.py`),
`docs/examples.html`, `docs/how-rumi-reads.html`, `examples/rumi-demo.ipynb` and
`references/python-api.md` in this skill.
