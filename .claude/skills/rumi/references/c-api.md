# C API

The public header is `core/include/rumi/rumi.h`; it includes `rumi_dtypes.def` and
`dlpack/dlpack.h`. Other sources: `core/src/capi.cpp`, `core/tests/test_c_header.c`,
`core/CMakeLists.txt`, and `bindings/python/rumi/_ffi.py`, whose hand-written cdef is the
reference caller. Both examples compile with `-std=c11 -Wall -Wextra` and ran against a
0.21.3 build.

## Contents

1. Stability
2. Building and linking
3. Conventions
4. Example: read a window
5. Example: frame layout before writing
6. Functions by task
7. Writing from C
8. Threads

## 1. Stability

- `RUMI_API_VERSION` is 1. Neither the source API nor the ABI is stable before 1.0, and
  the version and SONAME stay at 1 until then, so recompile after every update.
- The C header is the surface for future R and Julia bindings; `test_c_header.c` keeps it
  valid C11.
- 0.20.0 removed `rumi_read_stack`, `rumi_read_stack_dlpack`, `rumi_index_file`,
  `rumi_read_geo` and `rumi_read_time`. Use `rumi_read_many`, `rumi_read_many_dlpack`
  and `rumi_info`.
- `core/include/rumi/rumi.hpp` is the internal C++ interface. It includes Karu's headers
  and is not installed; do not build applications on it.

## 2. Building and linking

```bash
make build                         # core/build/librumi.{so,dylib}
make install PREFIX=/opt/rumi      # lib/, include/rumi/rumi.h, rumi_dtypes.def, include/dlpack/dlpack.h
cc -std=c11 app.c -I core/include -L core/build -lrumi -Wl,-rpath,"$PWD/core/build"
```

- `librumi` links GeoZL, OpenZL and Karu statically and exports only `RUMI_API`
  symbols. Building needs CMake 3.21, a C++23 compiler, Ninja by default, and libcurl
  7.83 or newer with OpenSSL 3 for Karu.
- The library's install name is `@rpath/librumi.1.dylib` on macOS (`librumi.so.1` on
  Linux). The renamed copy in `bindings/python/rumi/_lib/` cannot satisfy that name at
  run time (`Library not loaded: @rpath/librumi.1.dylib`); link against `core/build` or an
  installed prefix.

## 3. Conventions

- Functions return `rumi_status` (`RUMI_OK` is 0). `rumi_last_error()` holds the message
  of the last status-returning call on the calling thread, until the next such call or
  `rumi_clear_error()`.
- Outputs are unchanged on error.
- `times` and `bands` are **1-based** `int` arrays in output order; `(NULL, 0)` selects
  all in file order. Windows are zero-based and ordered `y_off, y_size, x_off, x_size`.
- `dst_size` is in bytes and must cover every decoded sample (one byte per sub-byte
  sample).

| Returned by | Release with |
| --- | --- |
| `rumi_spec_parse` | `rumi_spec_destroy` (the input blob may be freed right away) |
| `rumi_source_file`, `rumi_source_memory` | `rumi_source_free`; a memory source borrows its buffer, which must outlive it |
| `rumi_info` | `rumi_metadata_free` |
| `rumi_plan_ranges`, `rumi_write` blob, `rumi_geokeys` directory | `rumi_free` |
| `rumi_read_dlpack`, `rumi_read_many_dlpack` | the tensor's deleter or `rumi_dlpack_free` |
| `rumi_dlpack_legacy` | `rumi_dlpack_legacy_free` (the wrapper owns the versioned tensor) |
| `rumi_dtype_table`, `rumi_default_pattern`, `rumi_axis_name`, `rumi_version_string` | static; never freed |

## 4. Example: read a window

Band index 2 of `a.rumi` (a `(4, 300, 300)` uint16 Image stored as `b h w`), rows 10 to
73 and columns 20 to 83. It printed the same sum as the equivalent `rumi.read` and one
range, `560 +89015`: a single-band read of a cell layout still fetches the whole cell.

```c
#include <stdio.h>
#include <stdlib.h>
#include <rumi/rumi.h>

int main(void)
{
    FILE* f = fopen("a.header", "rb");
    unsigned char blob[4096];
    size_t size = fread(blob, 1, sizeof blob, f);
    fclose(f);

    rumi_spec* spec = NULL;
    rumi_source* src = NULL;
    if (rumi_spec_parse(blob, size, &spec) != RUMI_OK
        || rumi_source_file("a.rumi", &src) != RUMI_OK) {
        fprintf(stderr, "%s\n", rumi_last_error());
        return 1;
    }

    rumi_header h;
    rumi_spec_header(spec, &h);
    printf("%ux%u bands=%u time=%u unit=%u dtype=%d\n", h.image_width,
           h.image_length, h.samples_per_pixel, h.time_count, h.frame_unit,
           (int)h.dtype);

    const int bands[] = {3};                 /* 1-based: band index 2 */
    uint16_t dst[64 * 64];
    if (rumi_read(src, spec, NULL, 0, bands, 1, 10, 64, 20, 64, "y x",
                  dst, sizeof dst) != RUMI_OK) {
        fprintf(stderr, "%s\n", rumi_last_error());
        return 1;
    }
    unsigned long long sum = 0;
    for (size_t i = 0; i < 64 * 64; ++i) sum += dst[i];
    printf("c sum %llu\n", sum);

    rumi_range* ranges = NULL;
    size_t n = 0;
    rumi_plan_ranges(spec, NULL, 0, bands, 1, 10, 64, 20, 64, &ranges, &n);
    for (size_t i = 0; i < n; ++i)
        printf("range %llu +%llu\n", (unsigned long long)ranges[i].offset,
               (unsigned long long)ranges[i].length);
    rumi_free(ranges);

    rumi_source_free(src);
    rumi_spec_destroy(spec);
    return 0;
}
```

`rumi_source_file` also takes URIs and VSI paths (`reading.md`), configured from the
environment like the Python API.

## 5. Example: frame layout before writing

```c
#include <stdio.h>
#include <rumi/rumi.h>

int main(void)
{
    rumi_frame_pattern pattern;
    uint8_t unit = 0;
    if (rumi_compile_frame_pattern("t b (row h) (col w) -> row col (t b h w)",
                                   &pattern) != RUMI_OK
        || rumi_frame_unit(&pattern, 4, 3, &unit) != RUMI_OK) {
        fprintf(stderr, "%s\n", rumi_last_error());
        return 1;
    }

    char name[16];
    rumi_unit_name(unit, 4, 3, name, sizeof name);
    uint32_t across = 0, down = 0;
    uint64_t frames = 0;
    rumi_frame_count(unit, 1000, 700, 512, 4, 3, &across, &down, &frames);
    printf("unit %u (%s), grid %ux%u, %llu frames\n", unit, name, across, down,
           (unsigned long long)frames);

    for (uint64_t i = 0; i < frames; ++i) {
        rumi_frame_at at;
        rumi_frame_locate(unit, 1000, 700, 512, 4, 3, i, &at);
        printf("frame %llu row %u col %u dims", (unsigned long long)i, at.row,
               at.col);
        for (int k = 0; k < at.ndim; ++k) printf(" %lld", (long long)at.dims[k]);
        printf("\n");
    }

    rumi_write_desc desc = {
        .image_width = 1000, .image_length = 700, .time_count = 3,
        .tile_size = 512, .samples_per_pixel = 4, .dtype = RUMI_DT_UINT16,
        .frame_unit = unit,
    };
    uint64_t base = 0;
    if (rumi_write_base_offset(&desc, &base) != RUMI_OK) {
        fprintf(stderr, "%s\n", rumi_last_error());
        return 1;
    }
    printf("first frame at byte %llu\n", (unsigned long long)base);
    return 0;
}
```

Output:

```text
unit 4 (t b h w), grid 2x2, 4 frames
frame 0 row 0 col 0 dims 3 4 512 512
frame 1 row 0 col 1 dims 3 4 512 488
frame 2 row 1 col 0 dims 3 4 188 512
frame 3 row 1 col 1 dims 3 4 188 488
first frame at byte 500
```

## 6. Functions by task

| Task | Functions |
| --- | --- |
| Versions | `rumi_api_version`, `rumi_version_string`, `rumi_openzl_format_version` |
| Limits and threads | `rumi_set_max_frame_bytes`, `rumi_get_max_frame_bytes` (0 restores 1 GiB), `rumi_set_num_threads`, `rumi_get_num_threads` |
| Sample types | `rumi_dtype` (`RUMI_DT_*`), `rumi_dtype_table`, `rumi_check_samples` |
| Frame patterns | `rumi_compile_frame_pattern`, `rumi_frame_unit`, `rumi_unit_name`, `rumi_unit_from_name`, `rumi_unit_index_axes`, `rumi_unit_indexes_bands`, `rumi_axis_name`, `rumi_frame_count`, `rumi_frame_locate` |
| Output layouts | `rumi_default_pattern`, `rumi_compile_layout` (shape, and strides indexed by `RUMI_OUT_N` to `RUMI_OUT_X`) |
| Headers and metadata | `rumi_spec_parse`, `rumi_spec_header`, `rumi_spec_destroy`, `rumi_info`, `rumi_metadata_free` |
| Sources | `rumi_source_file`, `rumi_source_memory`, `rumi_source_free` |
| Planning without I/O | `rumi_plan_ranges` (unique ranges in frame-index order) |
| Reading | `rumi_read`, `rumi_read_dlpack`, `rumi_read_many`, `rumi_read_many_dlpack`, `rumi_dlpack_free`, `rumi_dlpack_legacy`, `rumi_dlpack_legacy_free` |
| Writing | `rumi_write`, `rumi_write_base_offset`, `rumi_geokeys` |

`rumi_read_many` takes an array of `rumi_read_item { source, spec, y_off, x_off }` plus
one shared `y_size` and `x_size`. Its default pattern keeps `n`, and a custom pattern
must include `n` for more than one item. `rumi_info(source, header, header_size, &out)`
accepts either input or both; `has_source` says whether time and georeferencing were
read.

## 7. Writing from C

- Fill `rumi_write_desc`: image and tile sizes, `samples_per_pixel`, `dtype`,
  `frame_unit` from `rumi_frame_unit`, `transform` (six doubles in `Affine` order) with
  `epsg`, or `NULL` and 0, `pixel_is_point`, and time as `time_type` (0 undefined,
  1 interval, 2 instant) with `time_count` or `2 * time_count` POSIX seconds.
- Pass `frame_count` payload pointers and sizes in frame-index order. `rumi_frame_locate`
  gives each frame's position and decoded `dims`; `perm` maps a cut ordered as
  (held band, held time, h, w) to those dims.
- `rumi_write` does not compress. Payloads come from GeoZL's C API or OpenZL and must
  decode as `writing.md` section 5 requires.
- On success `*out_blob` holds the header (release with `rumi_free`); on failure the
  partial file is removed and the outputs are untouched.

## 8. Threads

- All read entry points may run concurrently and share parsed specs and sources.
  Destination buffers must not overlap, and borrowed memory must outlive the call.
- The process-wide pool is sized by `rumi_set_num_threads` or `RUMI_NUM_THREADS` before
  the first parallel read; the first parallel read fixes it. A forked child starts with
  its own setting.
