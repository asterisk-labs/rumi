# Changelog

Notable user-visible changes are recorded here.

## [Unreleased]

### Breaking

- RUMI files now start with a 16-byte `RUMI` header instead of a BigTIFF
  header. TIFF readers will no longer open them accidentally. The header size
  and `base_frame_offset` are unchanged.
- The C layout and read APIs now support time axes. Layout strides are keyed by
  axis role, read functions accept time selections, and frame helpers receive
  the time count and step.
- `rumi_read`, `rumi_read_stack`, their DLPack variants, and `rumi.read` no
  longer accept `num_threads`. Configure the process-wide pool with
  `rumi_set_num_threads` or `rumi.set_num_threads` before the first parallel
  read.
- `rumi.read_time` now returns `Time(steps, kind)` instead of a bare list.

### Added

- Time-aware Cubes with shape `(T, B, Y, X)`. Patterns may use `t`,
  `rumi.write` accepts `time=`, `rumi.read` accepts `t=`, and Cube stacks use
  `(N, T, B, Y, X)` by default.
- `RumiHeader.time_count` and `RumiHeader.index_order`.
- `rumi_read_geo` and `rumi.read_geo` for reading the transform, EPSG code, and
  pixel anchor stored in a file.
- Windowed reads for padded sub-byte sample types.
- `rumi.read` accepts the descriptive selectors `time`, `bands`, and `window`.

### Changed

- `frame_unit` now selects one of ten registered axis layouts. Singleton band
  and time axes are omitted, and the frame index preserves the layout's band
  and time order.
- `rumi.frames` no longer requires a band axis when the input has none.
- Time coordinates cross the C API as POSIX seconds. The core selects the only
  canonical storage scale: days for whole-day coordinates, seconds otherwise.
- Format decisions and validation now live in the core. Bindings only convert
  language-level values, selections, and array views.

### Fixed

- Time trailers now receive full canonical validation, including coordinate
  order, prediction fields, residual padding, storage scale, and allocation
  limits. Fractional seconds are rejected instead of truncated.
- Time selections now report the selected shape. Large trailers use 64-bit
  seeks, huge implicit selections are bounded, and sub-byte stacks decode
  through the NumPy-backed path.
- Frame-range planning now follows physical frame order, removes duplicates,
  and avoids quadratic searches for large band counts.
- Boolean arrays round-trip as the padded `binary` type. Sub-byte padding is
  checked both before compression and after decoding.
- Frame counts, decoded sizes, coordinate arrays, and planned ranges now use
  checked arithmetic before allocation.
- Georeferencing validation now enforces the complete transformation matrix,
  canonical GeoKey entries, and the correct undefined-CRS model. Transforms
  shorter than six coefficients are rejected.
- The writer now removes partial files after any failure and rebuilds the
  returned header from the file it wrote.
- Header indexing now rejects truncated `SampleFormat` values, non-zero unused
  bytes and bits, and other non-canonical encodings.
- `rumi_spec_header` now reports `time_count`; the public C header, binding, and
  loaded library are checked for version drift.
- `FrameTable.attach` now repeats cell metadata across every band and time
  frame. `Frame.band` and `Frame.tile` preserve their specific errors.

## [0.17.0] - 2026-08-24

### Breaking

- RUMI 0.17 establishes a new compatibility baseline. Earlier files are not
  supported.
- The fixed IFD has 13 tags and is 276 bytes. The external header is 32 bytes,
  and `base_frame_offset` is `292 + external`.
- `rumi.frames` takes an axis pattern instead of `unit`:

      rumi.frames(image, "b (row h) (col w) -> row col (b h w)", tile_size=512)

  `RumiHeader.frame_unit` returns an axis order such as `"b h w"` rather than
  `"tile"` or `"cell"`.

### Added

- Frame layouts `b h w`, `h w b`, and `h w`, including inputs whose axes are
  not already ordered as `(B, Y, X)`.
- Core APIs for compiling frame patterns, naming layouts, counting frames, and
  locating each frame's position and decoded shape.
- `TimeCount` and an undefined 28-byte time trailer. Version 0.17 writes one
  unlabelled time step.
- A configurable decoded-frame allocation limit, one GiB by default.

### Changed

- Frame-pattern parsing and frame placement moved from Python into the core.
- Files record the complete frame axis order. Edge frames remain clipped when
  image dimensions are not divisible by the tile size. Specification 0.3.0.

### Fixed

- Frame layouts are validated against their band count, and sub-byte samples
  must have zero in every unused high bit.
- Stacks with incompatible frame layouts are rejected instead of risking an
  oversized scratch read.

## [0.16.0] - 2026-08-18

### Breaking

- RUMI 0.16 establishes a new compatibility baseline. Earlier files are not
  supported.

### Added

- GeoZL's `pfor` lossless terminal and integer `STORE=INDEX` support for
  `quant_sqrt`.

### Changed

- GeoZL 0.14.0 is now required for Python writing
  (`geozl>=0.14.0,<0.15`). Its integer `quant_linear` encoding may change
  compressed bytes, sizes, checksums, and content-addressed keys. Lossless
  frames are unaffected.
- `LOG:...,STORE=INDEX` now rejects integer input instead of ignoring it.

## [0.15.0] - 2026-08-16

### Breaking

- The external header has a canonical 28-byte layout; older headers are
  incompatible.
- `Frame`, `FrameTable`, and `rumi.frames` replace the Python tile API. Files
  now use the `.rumi` extension.
- The C API and shared-library SONAME advance to version 2.

### Added

- Tile and cell layouts, thread controls, stateless byte-range planning,
  Python 3.11–3.14 support, and sanitizer/fuzz coverage.

### Changed

- GeoZL 0.13.1 is required for Python writing. Parallel reads share a bounded
  worker pool, and the project is marked beta.

### Fixed

- Forked data-loader workers start with a valid pool, cell frames decode once
  per grid position, and malformed or oversized indexes are rejected earlier.

## [0.14.0] - 2026-08-12

### Added

- Independent C++ and Python format-conformance tests.

### Changed

- The repository and guide were reorganized around the C core and Python
  bindings.

## [0.13.0] - 2026-08-10

### Removed

- GDAL was removed from the runtime and packaging path.

## [0.12.0] - 2026-08-09

### Added

- A native BigTIFF writer, the high-level Python frame/write API,
  version-negotiated DLPack output, and a dtype table shared by C and Python.

### Changed

- Reads use positional I/O and validate payload bounds before allocation.
- Release wheels target Linux x86-64 and macOS arm64.

[Unreleased]: https://github.com/asterisk-labs/rumi/compare/v0.17.0...HEAD
[0.17.0]: https://github.com/asterisk-labs/rumi/compare/v0.16.0...v0.17.0
[0.16.0]: https://github.com/asterisk-labs/rumi/compare/v0.15.0...v0.16.0
[0.15.0]: https://github.com/asterisk-labs/rumi/compare/v0.14.0...v0.15.0
[0.14.0]: https://github.com/asterisk-labs/rumi/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/asterisk-labs/rumi/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/asterisk-labs/rumi/releases/tag/v0.12.0
