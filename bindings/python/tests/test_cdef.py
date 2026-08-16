"""Keep CFFI declarations and the public C source API from drifting.

CFFI loads RUMI in ABI mode, so it cannot detect a copied signature that no
longer matches the header. A mismatch can corrupt memory instead of raising a
useful exception.
"""

import ast
import re
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[3]
_HEADER = _ROOT / "core" / "include" / "rumi" / "rumi.h"
_DTYPES = _HEADER.with_name("rumi_dtypes.def")
_FFI_SOURCE = _ROOT / "bindings" / "python" / "rumi" / "_ffi.py"

if not _HEADER.exists():
    pytest.skip("no C header next to the package", allow_module_level=True)

# A declaration, not a definition. Return types may end in one or more stars
# and may be placed on the line before the function name.
_DECL = re.compile(
    r"([A-Za-z_][\w \t\n]*?)\s*((?:\*\s*)*)(\w+)\s*\(([^;{)]*)\)\s*;",
    re.MULTILINE,
)
_DROP = re.compile(r"\b(RUMI_API|restrict|__restrict|const|static|inline)\b")
_NOT_A_TYPE = {"return", "if", "while", "for", "switch", "sizeof", "typedef"}
_TYPEDEF_BLOCK = re.compile(
    r"typedef\s+(enum|struct)\s*(?:\w+)?\s*\{(.*?)\}\s*(\w+)\s*;",
    re.DOTALL,
)

# Public functions present when the C API becomes source-stable in 0.15. New
# functions may be added, but these names and signatures remain available.
_PUBLIC_API_0_15 = r"""
int rumi_api_version(void);
const char* rumi_version_string(void);
int rumi_openzl_format_version(void);
const char* rumi_last_error(void);
void rumi_clear_error(void);
void rumi_free(void* ptr);
int rumi_set_num_threads(int n);
int rumi_get_num_threads(void);
rumi_status rumi_index_file(const char* path, unsigned char** out_blob,
                            size_t* out_size);
size_t rumi_dtype_table(const rumi_dtype_info** out);
rumi_status rumi_compile_layout(const char* pattern, int64_t n, int64_t b,
                                int64_t y, int64_t x, rumi_layout* out);
rumi_status rumi_spec_parse(const unsigned char* blob, size_t blob_size,
                            rumi_spec** out);
void rumi_spec_destroy(rumi_spec* spec);
rumi_status rumi_spec_header(const rumi_spec* spec, rumi_header* out);
rumi_status rumi_source_file(const char* path, rumi_source** out);
rumi_status rumi_source_memory(const void* data, size_t size,
                               rumi_source** out);
void rumi_source_free(rumi_source* src);
rumi_status rumi_plan_ranges(const rumi_spec* spec, const int* bands,
                             size_t n_bands, int y_off, int y_size, int x_off,
                             int x_size, rumi_range** out, size_t* out_count);
rumi_status rumi_read(rumi_source* src, const rumi_spec* spec,
                      const int* bands, size_t n_bands, int y_off, int y_size,
                      int x_off, int x_size, const char* pattern,
                      int num_threads, void* dst, size_t dst_size);
rumi_status rumi_read_stack(rumi_source* const* sources,
                            const rumi_spec* const* specs, size_t n_images,
                            const int* n_index, size_t n_n, const int* bands,
                            size_t n_bands, int y_off, int y_size, int x_off,
                            int x_size, const char* pattern, int num_threads,
                            void* dst, size_t dst_size);
rumi_status rumi_read_dlpack(rumi_source* src, const rumi_spec* spec,
                             const int* bands, size_t n_bands, int y_off,
                             int y_size, int x_off, int x_size,
                             const char* pattern, int num_threads,
                             DLManagedTensorVersioned** out);
rumi_status rumi_read_stack_dlpack(rumi_source* const* sources,
                                   const rumi_spec* const* specs,
                                   size_t n_images, const int* n_index,
                                   size_t n_n, const int* bands,
                                   size_t n_bands, int y_off, int y_size,
                                   int x_off, int x_size, const char* pattern,
                                   int num_threads,
                                   DLManagedTensorVersioned** out);
void rumi_dlpack_free(DLManagedTensorVersioned* t);
DLManagedTensor* rumi_dlpack_legacy(DLManagedTensorVersioned* t);
void rumi_dlpack_legacy_free(DLManagedTensor* t);
rumi_status rumi_write(const char* path, const rumi_write_desc* desc,
                       const unsigned char* const* frames,
                       const size_t* sizes, size_t frame_count,
                       unsigned char** out_blob, size_t* out_size);
rumi_status rumi_write_base_offset(const rumi_write_desc* desc, uint64_t* out);
rumi_status rumi_geokeys(uint32_t epsg, int pixel_is_point,
                         unsigned char** out_dir, size_t* out_dir_size,
                         unsigned char** out_dbl, size_t* out_dbl_size,
                         unsigned char** out_ascii, size_t* out_ascii_size);
"""

_PUBLIC_TYPES_0_15 = r"""
typedef enum {
    RUMI_OK = 0, RUMI_ERR_INVALID = 1, RUMI_ERR_IO = 2,
    RUMI_ERR_PARSE = 3, RUMI_ERR_FORMAT = 4, RUMI_ERR_DECODE = 5,
    RUMI_ERR_OOM = 6, RUMI_ERR_UNSUPPORTED = 7, RUMI_ERR_INTERNAL = 99
} rumi_status;
typedef struct {
    uint8_t code; uint8_t sample_format; uint8_t bits; uint8_t dl_code;
    uint8_t dl_bits; const char* name;
} rumi_dtype_info;
typedef struct {
    uint32_t image_width; uint32_t image_length; uint16_t tile_width;
    uint16_t tile_length; uint16_t samples_per_pixel; uint8_t bits_per_sample;
    uint8_t sample_format; uint8_t frame_unit; uint32_t tiles_across;
    uint32_t tiles_down; uint64_t base_frame_offset; rumi_dtype dtype;
} rumi_header;
typedef struct {
    int64_t shape[4]; int ndim; int64_t sn; int64_t sb; int64_t sy;
    int64_t sx; int native;
} rumi_layout;
typedef struct { uint64_t offset; uint64_t length; } rumi_range;
typedef struct {
    uint32_t image_width; uint32_t image_length; uint16_t tile_size;
    uint16_t samples_per_pixel; rumi_dtype dtype; const double* transform;
    uint32_t epsg; int pixel_is_point; uint8_t frame_unit;
} rumi_write_desc;
"""

_DTYPES_0_15 = {
    "UNKNOWN": 0,
    "UINT8": 1, "INT8": 2, "UINT16": 3, "INT16": 4,
    "UINT32": 5, "INT32": 6, "UINT64": 7, "INT64": 8,
    "FLOAT16": 9, "FLOAT32": 10, "FLOAT64": 11,
    "CINT16": 12, "CINT32": 13,
    "CFLOAT16": 14, "CFLOAT32": 15, "CFLOAT64": 16,
    "FLOAT8_E4M3FN": 17, "FLOAT8_E5M2": 18, "BFLOAT16": 19,
    "UINT4": 20, "INT4": 21, "UINT2": 22, "INT2": 23, "BINARY": 24,
    "FLOAT8_E8M0": 25, "FLOAT6_E2M3": 26, "FLOAT6_E3M2": 27,
    "FLOAT4_E2M1": 28,
}


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    # A multiline return-type regex must not absorb #define tokens that happen
    # to precede a declaration.
    kept = []
    skipping_directive = False
    for line in text.splitlines():
        if skipping_directive or line.lstrip().startswith("#"):
            skipping_directive = line.rstrip().endswith("\\")
            continue
        kept.append(line)
    return "\n".join(kept)


def _tokens(text: str) -> list[str]:
    """Make stars separate tokens so `void *x` and `void* x` agree."""
    return re.sub(r"\*", " * ", _DROP.sub(" ", text)).split()


def _params(text: str) -> tuple[str, ...]:
    text = re.sub(r"\b(\w+)\s*\[[^\]]*\]", r"* \1", text)
    out = []
    for part in text.split(","):
        tokens = _tokens(part)
        if not tokens or tokens == ["void"]:
            continue
        if len(tokens) > 1 and re.fullmatch(r"\w+", tokens[-1]):
            tokens = tokens[:-1]
        out.append(" ".join(tokens))
    return tuple(out)


def _declarations(text: str) -> dict[str, tuple[str, tuple[str, ...]]]:
    found = {}
    for return_type, stars, name, params in _DECL.findall(_strip_comments(text)):
        tokens = _tokens(return_type) + _tokens(stars)
        if not tokens or tokens[0] in _NOT_A_TYPE:
            continue
        found[name] = (" ".join(tokens), _params(params))
    return found


def _typedef_blocks(text: str) -> dict[str, tuple[str, str]]:
    found = {}
    for kind, body, name in _TYPEDEF_BLOCK.findall(_strip_comments(text)):
        normalized = re.sub(r"([*;{},\[\]=])", r" \1 ", _DROP.sub(" ", body))
        found[name] = (kind, " ".join(normalized.split()))
    return found


def _cdef_text() -> str:
    tree = ast.parse(_FFI_SOURCE.read_text())
    for node in tree.body:
        if isinstance(node, ast.Assign):
            if any(isinstance(target, ast.Name) and target.id == "_CDEF"
                   for target in node.targets):
                value = ast.literal_eval(node.value)
                assert isinstance(value, str)
                return value
    raise AssertionError("_ffi.py has no literal _CDEF assignment")


@pytest.fixture(scope="module")
def c_declarations():
    return _declarations(_HEADER.read_text())


@pytest.fixture(scope="module")
def cdef_declarations():
    return _declarations(_cdef_text())


def test_the_cdef_is_not_empty(cdef_declarations, c_declarations):
    assert len(cdef_declarations) >= 20
    assert len(c_declarations) > len(cdef_declarations)


def test_every_declared_symbol_exists_in_the_header(cdef_declarations,
                                                    c_declarations):
    missing = sorted(set(cdef_declarations) - set(c_declarations))
    assert not missing, f"declared in _ffi.py but not rumi.h: {missing}"


def test_every_cdef_signature_matches_the_header(cdef_declarations,
                                                 c_declarations):
    drift = []
    for name, signature in sorted(cdef_declarations.items()):
        current = c_declarations.get(name)
        if current is not None and signature != current:
            drift.append(f"{name}\n    C  {current}\n    py {signature}")
    assert not drift, "cdef drifted from rumi.h:\n" + "\n".join(drift)


def test_cdef_public_types_match_the_header():
    selected = set(_typedef_blocks(_PUBLIC_TYPES_0_15))
    header_types = _typedef_blocks(_HEADER.read_text())
    cdef_types = _typedef_blocks(_cdef_text())
    drift = [
        f"{name}: C {header_types.get(name)}, py {cdef_types.get(name)}"
        for name in sorted(selected)
        if header_types.get(name) != cdef_types.get(name)
    ]
    assert not drift, "cdef type layouts drifted from rumi.h:\n" + "\n".join(drift)


def test_public_c_api_keeps_the_0_15_signatures(c_declarations):
    baseline = _declarations(_PUBLIC_API_0_15)
    assert len(baseline) == 28
    drift = []
    for name, signature in baseline.items():
        current = c_declarations.get(name)
        if current != signature:
            drift.append(f"{name}: expected {signature}, found {current}")
    assert not drift, "public C API changed:\n" + "\n".join(drift)


def test_public_c_types_keep_the_0_15_layouts_and_values():
    baseline = _typedef_blocks(_PUBLIC_TYPES_0_15)
    current = _typedef_blocks(_HEADER.read_text())
    assert len(baseline) == 6
    drift = [
        f"{name}: expected {signature}, found {current.get(name)}"
        for name, signature in baseline.items()
        if current.get(name) != signature
    ]
    assert not drift, "public C types changed:\n" + "\n".join(drift)

    api_version = re.search(r"^#define RUMI_API_VERSION\s+(\d+)$",
                            _HEADER.read_text(), re.MULTILINE)
    assert api_version and api_version[1] == "2"


def test_dtype_codes_from_0_15_are_append_only():
    current = {"UNKNOWN": 0}
    for code, symbol in re.findall(
        r"RUMI_DTYPE\(\s*(\d+)\s*,\s*(\w+)", _DTYPES.read_text()
    ):
        current[symbol] = int(code)
    drift = {
        symbol: (code, current.get(symbol))
        for symbol, code in _DTYPES_0_15.items()
        if current.get(symbol) != code
    }
    assert not drift, f"released dtype codes changed: {drift}"


def test_the_parser_sees_a_planted_change(c_declarations):
    planted = _declarations(
        "rumi_status rumi_write_base_offset(const rumi_write_desc* desc, "
        "uint32_t* out);"
    )
    assert planted["rumi_write_base_offset"] != c_declarations["rumi_write_base_offset"]
