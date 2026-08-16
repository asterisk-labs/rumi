# Changelog

User-visible changes are recorded here.

## [Unreleased]

## [0.15.0] - 2026-08-16

### Breaking

- The external RUMI header now has a canonical 28-byte layout. Headers from
  0.14 and earlier are incompatible.
- The Python tile API is replaced by `Frame`, `FrameTable`, and `rumi.frames`.
  RUMI files now use the `.rumi` extension.
- The C API version and shared-library SONAME advance to 2 after changes to the
  public header and write descriptor.

### Added

- Tile and cell frame layouts. Tile frames hold one band; cell frames hold all
  bands at one grid position.
- Read thread controls in C and Python, including `RUMI_NUM_THREADS` and
  `ALL_CPUS`.
- Stateless byte-range planning from the external RUMI header.
- Format conformance, sanitizer, and fuzz coverage for headers, raster indexing,
  and release artifacts.
- Python 3.11 through 3.14 support.
- The specification website and technical deck.

### Changed

- GeoZL is updated to 0.13.1. Python writing requires
  `geozl>=0.13.1,<0.14`.
- Parallel reads now share a bounded worker pool.
- The project is now marked as beta.

### Fixed

- Forked data-loader workers start with a valid thread pool.
- Cell frames are decoded once per grid position, not once per band.
- Malformed and non-canonical files are rejected more consistently.
- Constant frame counts stay compact. Variable-count indexes are bounded before
  allocation.

## [0.14.0] - 2026-08-12

### Added

- Independent C++ and Python format conformance tests.

### Changed

- The repository and guide were reorganized around the C core and Python
  bindings.

## [0.13.0] - 2026-08-10

### Removed

- GDAL was removed from the runtime and packaging path.

## [0.12.0] - 2026-08-09

### Added

- A native BigTIFF writer and the high-level Python frame/write API.
- Version-negotiated DLPack output and a single dtype table shared by C and
  Python.

### Changed

- RUMI reads use positional I/O and validate payload bounds before allocation.
- Release wheels target Linux x86-64 and macOS arm64.

[Unreleased]: https://github.com/asterisk-labs/rumi/compare/v0.15.0...HEAD
[0.15.0]: https://github.com/asterisk-labs/rumi/compare/v0.14.0...v0.15.0
[0.14.0]: https://github.com/asterisk-labs/rumi/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/asterisk-labs/rumi/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/asterisk-labs/rumi/releases/tag/v0.12.0
