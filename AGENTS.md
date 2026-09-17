# Rumi repository guidance

Rumi is a raster format for machine-learning datasets: a C++23 core with a C API and
CFFI Python bindings, defined by `SPEC.md`. It links GeoZL, and through it OpenZL, and
the Karu transport from the submodules in `extern/`.

## Working agreements

- `SPEC.md` is normative. A change to the file layout, header blob, time trailer or a
  registry updates the specification, the independent parsers and golden digests in the
  Python tests, and the fuzz seeds together, and is recorded in `CHANGELOG.md`.
- Keep format decisions and validation in `core/`. Bindings only convert language
  values, selections and array views.
- Registries are append-only. Never renumber `rumi_dtypes.def` codes or `frame_unit`
  values.
- Readers process untrusted headers, files and frames. Check sizes, counts, offsets and
  arithmetic before allocating or copying, and fail before exceeding resource limits.
- Keep `rumi.h` valid C11, and keep the cdef in `bindings/python/rumi/_ffi.py` and the
  signatures recorded in `test_cdef.py` in step with it.
- Treat `extern/geozl` and `extern/karu` as upstream submodules. Move their pinned
  revisions only when the task requires an upgrade, together with the Python extras,
  `NOTICE` and the changelog; `tools/check_release.py` checks them.
- Treat `build*/`, `core/build*`, `_site/`, `dist/`, `fuzz/out/`, `fuzz/corpus/` and the
  staged library under `bindings/python/rumi/_lib/` as build output. `docs/`, `deck/` and
  `img/` are hand-maintained sources.
- Keep comments short, and match the density of the neighbouring code.

## Validation

- Iterate with the narrowest tests, then run `make test` and `make lint` for changes
  that affect behavior across the core and the bindings.
- Run `make ctest` for core changes, and the sanitizer and fuzz targets for parser,
  planning, decode, buffer or threading changes.
- Run `make docs` after editing `SPEC.md`, `img/` or the site.

Detailed usage, format, C API, compatibility and debugging knowledge lives in the
repository's `rumi` skill. Load the relevant reference from that skill instead of
expanding this always-on file with task-specific instructions.
