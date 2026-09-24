# Working on the rumi repository

For changes inside `github.com/asterisk-labs/rumi`. Sources: `Makefile`,
`core/CMakeLists.txt`, `.github/workflows/`, `.githooks/pre-commit`, `tools/`,
`docs/README.md`, `bindings/python/pyproject.toml` and the test suites.

## Contents

1. Repository map
2. Setup and build
3. Make targets
4. Test suites and what they guard
5. Sanitizers and fuzzing
6. Documentation site
7. Common changes
8. CI and releases
9. Conventions

## 1. Repository map

```text
core/
  include/rumi/rumi.h            public C API, kept valid C11
  include/rumi/rumi_dtypes.def   sample type registry (X-macro), append-only
  include/rumi/epsg_kinds.def    projected and geographic EPSG ranges, generated
  include/rumi/rumi.hpp          internal C++ interface: UNIT_REGISTRY, Header, TimeAxis
  include/rumi/thread_pool.hpp   the process-wide read pool
  include/dlpack/dlpack.h        vendored DLPack
  src/pattern.cpp                frame and output pattern compilers, frame geometry
  src/parser.cpp                 header blob parsing
  src/error.cpp                  error messages of any length
  src/builder.cpp                file indexing: IFD, trailer, header rebuild
  src/write.cpp                  the writer
  src/geokeys.cpp src/time.cpp   georeferencing and the time trailer
  src/read.cpp src/plan.cpp      selections, planning, decode tasks and frame checks
  src/source.cpp                 Karu transport and memory sources
  src/dlpack.cpp src/capi.cpp    DLPack tensors and the C ABI
  tests/test_core.cpp            C++ component tests (rumi_tests)
  tests/test_c_header.c          C11 compile check of rumi.h
bindings/python/
  rumi/_ffi.py                   hand-written cffi cdef (ABI mode) and library loading
  rumi/_frames.py _pattern.py _write.py _read.py _info.py _time.py _threads.py _dtype.py _repr.py
  rumi/_lib/                     staged librumi (ignored by git)
  tests/                         pytest suites
  pyproject.toml hatch_build.py check_wheel.py
fuzz/                            libFuzzer harnesses: header, index, pattern, read; replay/ seeds in git
docs/                            hand-written static site; SPEC.md is rendered in by make docs
deck/ img/                       slide deck and SVG figures, mounted under the site
examples/rumi-vs-geotiff.ipynb   GeoTIFF decompress comparison notebook
tools/                           build_docs.py check_release.py check_editable.py make_fuzz_seeds.py
                                 gen_epsg_kinds.py benchmark_dataloader.py build_static_curl.sh
extern/geozl extern/karu         submodules; GeoZL brings OpenZL
licenses/ NOTICE                 third-party notices shipped with wheels
VERSION CHANGELOG.md SPEC.md COMPATIBILITY.md SECURITY.md
```

`build*/`, `_site/`, `dist/`, `*.rumi` and `*.tif` are ignored build output or local data.

## 2. Setup and build

Requirements: Python 3.11+, Git, Make, CMake 3.21+, Ninja, a C++23 and C11 compiler,
and libcurl 7.83+ with OpenSSL 3 development files (`libcurl4-openssl-dev libssl-dev`
on Ubuntu).

```bash
git submodule update --init --recursive      # or: make submodules
python -m pip install cmake ninja numpy cffi pytest ruff mypy
make python      # configure, build librumi, stage it, pip install -e, verify the install
make test        # make python, then pytest bindings/python
make lint        # ruff and mypy with bindings/python/pyproject.toml
make ctest       # C++ component tests in core/build-tests
```

- `make python` ends with `tools/check_editable.py`, which fails when `import rumi` loads
  another checkout or when `VERSION`, the package metadata and `rumi_version_string()`
  disagree.
- Changing `VERSION` reconfigures CMake, so the library reports the new version.
- Variables: `BUILD_TYPE=Debug`, `PYTHON=python3.12`, `GEN='Unix Makefiles'`,
  `CMAKE_FLAGS=...`, `PREFIX=/opt`.
- Without GeoZL installed, the tests that write files skip.
- One test: `python -m pytest bindings/python/tests/test_read.py -q -k window`.

## 3. Make targets

| Target | What it does |
| --- | --- |
| `make` / `make python` | build, stage `librumi` into `bindings/python/rumi/_lib`, editable install, check |
| `make build` | build `core/build/librumi` only |
| `make lib` | build and stage the library (the CI entry) |
| `make test` | `make python`, then pytest |
| `make lint` | Ruff and mypy |
| `make ctest` | configure `core/build-tests` with `RUMI_BUILD_TESTS=ON` and run `rumi_tests` |
| `make fuzz-check` | build fuzzers with Clang, seed, run each for `FUZZ_TIME` seconds, fail on findings |
| `make fuzz-replay` | replay `fuzz/replay/` and any cached corpus once |
| `make docs` | render `SPEC.md` into a deployable copy of `docs/` in `_site/` |
| `make install` | `cmake --install` into `PREFIX` |
| `make sync` | validate `VERSION` (and write R metadata once `bindings/r` exists) |
| `make clean` | remove builds, caches, staged libraries and fuzz output |

## 4. Test suites and what they guard

| Suite | Guards |
| --- | --- |
| `test_readme.py` | every README Python block compiles; the quick start runs and round-trips |
| `test_spec.py` | files against `SPEC.md` with an independent parser: tags, placement, base offset, georeferencing, trailer canonical rules, rejections |
| `test_writer.py` | writer output, golden file digests (`GOLDEN`), writer refusals |
| `test_independent_reader.py` | a pure-Python reader built from the specification reads what the C reader reads |
| `test_cube.py`, `test_order.py` | frame units, index order against the specification, time selections |
| `test_pattern.py` | the pattern compiler through Python |
| `test_read.py`, `test_many.py` | reads, batches, DLPack exactly-once, remote paths against a local HTTP server, DataLoader batching |
| `test_time.py`, `test_info.py`, `test_repr.py` | `time=` conversion, `Metadata`, table and notebook output |
| `test_threads.py` | pool sizing, pinning, fork and DataLoader workers |
| `test_cdef.py` | the cdef against `rumi.h`, recorded public signatures and struct layouts, append-only dtype codes |
| `test_editable.py` | `tools/check_editable.py` |
| `test_skill.py` | this skill names the release in `VERSION`, `.agents/skills/rumi` points at it, and its links resolve |
| `core/tests/test_core.cpp` | C++ components, built as `rumi_tests` |

## 5. Sanitizers and fuzzing

```bash
cmake -S core -B core/build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRUMI_BUILD_TESTS=ON -DRUMI_SANITIZE=address,undefined
cmake --build core/build-asan && ctest --test-dir core/build-asan --output-on-failure
```

- CI runs the core tests three ways with GCC: warnings as errors, ASan with UBSan, and
  TSan (`-DRUMI_SANITIZE=thread` skips the post-fork thread test).
- `make fuzz-check FUZZ_TIME=60` builds `rumi_{header,index,pattern,read}_fuzzer` with
  Clang, ASan and UBSan (Homebrew LLVM on macOS, since Apple clang lacks libFuzzer).
  Findings land in `fuzz/out/`; `make fuzz-replay` reruns the versioned inputs.
- `python tools/make_fuzz_seeds.py` regenerates `fuzz/replay/` with valid frames after a
  format change.
- Run sanitizers and the fuzzers for parser, planning, decode and buffer changes.
  Crashes, hangs, invalid memory access or silent corruption from malformed input are
  security issues (`SECURITY.md`).

## 6. Documentation site

- `docs/` is the website source: `index.html` (routes as templates),
  `how-rumi-reads.html` (a source tour), `examples.html`, and assets. It has no framework.
- `python -m pip install -r docs/requirements.txt`, then `make docs` and
  `python -m http.server 8000 --directory _site`.
- The build renders `SPEC.md` into the `_site/index.html` spec template, mounts `img/`
  and `deck/`, checks local links and assets, and fails when figures in `img/` quote a
  header size or tag count that disagrees with `SPEC.md`.
- `docs/index.html` keeps its own copy of the rendered specification so the page works
  when opened directly; refresh that region after editing `SPEC.md`.
- `pages.yml` builds the site for every push and pull request and deploys `main`.

## 7. Common changes

- **C API function:** declare it in `rumi.h` with `RUMI_API` and a short comment,
  implement it in `capi.cpp`, add it to the cdef in `_ffi.py` and to `_PUBLIC_API` in
  `test_cdef.py`, then wrap and test it in Python.
- **Sample type:** append a row to `rumi_dtypes.def` (never renumber), extend the Sample
  encodings table in `SPEC.md`, map its DLPack code in `_dtype.py`, and cover it in
  `test_writer.py` and `test_spec.py`.
- **EPSG table:** `python tools/gen_epsg_kinds.py > core/include/rumi/epsg_kinds.def`
  (needs pyproj).
- **GeoZL update:** move `extern/geozl` to the release tag, update the `write` and `test`
  extras, `NOTICE` and `CHANGELOG.md`, then run `python tools/check_release.py` and the
  full suite.
- **Karu update:** move `extern/karu`, update `NOTICE` (`Karu X.Y.Z`),
  `licenses/LICENSE.Karu` if it changed, and `CHANGELOG.md`.
- **Format change:** follow `compatibility.md` section 5.

## 8. CI and releases

- `ci.yml`: the core matrix above, and Python 3.11 to 3.14 on Linux running
  `make python`, `make lint` (3.12), `check_release.py` (3.12) and pytest, with CPU PyTorch
  on 3.12 for the fork tests.
- `fuzz.yml`: path-filtered pushes and pull requests plus a nightly run.
- `release.yml` runs on `v*` tags (a manual run builds without publishing): validate
  metadata against the tag, build `librumi` for linux-x86_64 (static curl) and
  macos-arm64, build, repair and test wheels, read a remote fixture without a CA
  override, check wheel tags, then create the GitHub Release and publish to PyPI.
- Release steps: bump `VERSION`; add `## [X.Y.Z] - YYYY-MM-DD` and its compare link to
  `CHANGELOG.md`; update `This skill describes **rumi X.Y.Z**` in `SKILL.md`; run
  `python tools/check_release.py`; commit (`Prepare Rumi X.Y.Z`), tag `vX.Y.Z` and push the
  tag.
- `git config core.hooksPath .githooks` enables a pre-commit hook that runs
  `check_release.py` whenever `VERSION`, `CHANGELOG.md`, `pyproject.toml` or
  `extern/geozl` is staged.

## 9. Conventions

- Format decisions and validation live in the core. Bindings only convert language
  values, selections and array views.
- Error messages say what was expected and what arrived, in plain sentences; tests match
  on their text.
- Comments are short and explain why, matching the density of neighbouring code.
- `CHANGELOG.md` follows Keep a Changelog, with `Breaking`, `Added`, `Changed`,
  `Performance`, `Removed` and `Fixed` sections as needed.
