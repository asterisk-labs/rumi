# Changelog

Notable user-visible changes are recorded here.

## [Unreleased]

## [0.21.0] - 2026-09-10

### Added

- `Metadata` displays every attribute it carries. Notebooks show an HTML table
  and tile grid; its text representation is aligned.

### Changed

- Updated Karu to 0.2.0. VSI paths and chaining are unchanged; HTTP and GCS
  configuration now use Karu-owned option names. Credential discovery for
  independent cloud paths no longer blocks the network scheduler.

### Fixed

- `make python` now verifies that the editable install loads this checkout and
  its matching native library instead of accepting an unrelated `rumi` module.

## [0.20.1] - 2026-09-09

### Fixed

- Linux wheels now discover the host system's CA bundle instead of retaining
  the certificate path from the build container.

## [0.20.0] - 2026-09-09

### Added

- `rumi.info(source=..., header=...)` is the single metadata operation. It can
  inspect either input and validates their synchronization when both are
  supplied. `Metadata` combines the external header, shape, dtype, tile and
  frame layout with time and georeferencing when a source is available.
- `rumi_info` exposes the same operation for C, R and Julia over any
  `rumi_source`, including paths, remote URIs and borrowed memory.

- `rumi.read_many` reads one fixed-size window per source:
  `windows[i]` is the `(row, column, height, width)` taken from `sources[i]`.
  Every window must be the same size, and items come back along the `n` axis
  in the order given, including a one-item call. Sources must agree on tile
  size, band count, dtype, time step count, and which of band and time a frame
  holds; image dimensions may differ.

- `rumi_read_many` and `rumi_read_many_dlpack` expose the same operation through
  an array of `rumi_read_item` structs. The struct keeps each source, header and
  window origin together so R and Julia bindings do not need parallel arrays.

- Local paths and remote object URIs now share `rumi_source_file` and the same
  read operations.
- The README documents the supported cloud URI/VSI paths and links to the
  transport configuration reference.

### Performance

- Remote frames are decoded as Karu completes them, overlapping network reads
  with CPU decoding while preserving item order. A decode failure cancels
  outstanding transfers.
- Successive reads on the same calling thread reuse Karu's connection pool
  while their complete transport configuration remains unchanged. Environment
  changes still take effect on the next operation.
- Full frames whose decoded byte order exactly matches the requested output now
  decode directly into the result. This removes the scratch-buffer copy for the
  common one-chip `b h w` training layout. Batched plan vectors also grow
  geometrically and reserve their plane offsets up front.

### Changed

- `rumi.read` and `rumi.read_many` now require a header for every source. Use
  `rumi.info(source=...).header` to rebuild one from an existing file.
- Updated the internal Karu transport with bounded range coalescing, strict
  response validation, safer credential refresh, and Source Cooperative
  paths. Each Rumi operation owns one client shared by all of its sources;
  sources retain only immutable locators, and no object data or metadata is
  cached.
- GitHub Actions are commit-pinned, time-bounded, and fetch the public Karu
  submodule without a deploy key or persisted checkout credentials.
- Buffer and DLPack forms of single and multi-item reads now share one C++
  preparation path for selections, default layouts, and output sizing. Python
  DLPack reads consume the shape produced by the core instead of compiling the
  layout a second time.
- The C header now states the concurrent-read and borrowed-buffer contract, and
  a C11 compile check protects the surface intended for future R and Julia
  bindings.

### Removed

- `rumi.chunks`; applications own their sampling policy and pass selections to
  `read` or `read_many`.
- `RumiHeader`, `Geo`, `Time`, `read_geo`, and `read_time`. Their overlapping
  metadata responsibilities are replaced by `Metadata` and `info`.
- `rumi_index_file`, `rumi_read_geo`, and `rumi_read_time`; `rumi_info` replaces
  all three without restricting metadata inspection to local paths.

- The Python read API no longer accepts `n`, `t`, `b`, `y`, or `x`. Use
  `time`, `bands`, and `window` for one source, and order the sources passed to
  `read_many` for the `n` axis.
- `rumi.read` no longer accepts a sequence of sources. `rumi.read_many` is the
  only multi-source Python operation; repeat a window to apply it to several
  sources.
- `rumi_read_stack`, `rumi_read_stack_dlpack`, and the C++ `read_stack` entry
  point were removed. The unstable C API remains at version 1 until Rumi 1.0.

### Fixed

- `RumiArray`, returned when `framework=None`, is now exported from the Python
  package as indicated by its representation and read documentation.
- `Metadata.tile` now reports rectangular tiles as `(height, width)`, and
  `Metadata.frames` uses both tile dimensions instead of assuming squares.
- Invalid read selections now return `RUMI_ERR_INVALID` instead of reporting
  an I/O failure.
- Remote range limits count only axes stored as separate frames, so a valid
  cell-frame plan is not rejected for ranges it would never allocate.
- Read-plan dimensions and vector growth use checked, portable arithmetic.
- Remote reads with an external header do not issue a preliminary size or
  existence request. Transport configuration is snapshotted independently for
  every `read`, `read_many`, or source-indexing operation.

## [0.19.0] - 2026-09-01

### Changed

- Updated to GeoZL 0.16.0 (`geozl>=0.16.0,<0.17` for Python writing), adding
  fused planar codecs and faster, lower-memory PFOR decoding. Frames from
  GeoZL 0.14 and 0.15 remain readable.
- GeoZL 0.15 and 0.15.1 also add optional `int32` frame coefficients and accept
  all-zero `quant_linear` domains. RUMI preserves coefficients without
  interpreting them.

## [0.18.0] - 2026-08-29

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
- `rumi.chunks` builds lazy, tile-aligned read plans for training samples.

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

[Unreleased]: https://github.com/asterisk-labs/rumi/compare/v0.21.0...HEAD
[0.21.0]: https://github.com/asterisk-labs/rumi/compare/v0.20.1...v0.21.0
[0.20.1]: https://github.com/asterisk-labs/rumi/compare/v0.20.0...v0.20.1
[0.20.0]: https://github.com/asterisk-labs/rumi/compare/v0.19.0...v0.20.0
[0.19.0]: https://github.com/asterisk-labs/rumi/compare/v0.18.0...v0.19.0
[0.18.0]: https://github.com/asterisk-labs/rumi/compare/v0.17.0...v0.18.0
[0.17.0]: https://github.com/asterisk-labs/rumi/compare/v0.16.0...v0.17.0
[0.16.0]: https://github.com/asterisk-labs/rumi/compare/v0.15.0...v0.16.0
[0.15.0]: https://github.com/asterisk-labs/rumi/compare/v0.14.0...v0.15.0
[0.14.0]: https://github.com/asterisk-labs/rumi/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/asterisk-labs/rumi/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/asterisk-labs/rumi/releases/tag/v0.12.0
