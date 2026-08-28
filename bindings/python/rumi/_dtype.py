from dataclasses import dataclass
from types import ModuleType

import numpy as np

from ._ffi import _check, ffi, lib

# DLPack type codes from dlpack.h.
_DL_INT, _DL_UINT, _DL_FLOAT, _DL_BFLOAT, _DL_COMPLEX = 0, 1, 2, 4, 5
_DL_F8_E4M3FN, _DL_F8_E5M2, _DL_F8_E8M0 = 10, 12, 14
_DL_F6_E2M3, _DL_F6_E3M2, _DL_F4_E2M1 = 15, 16, 17

_NUMPY_SCALARS: dict[tuple[int, int], type] = {
    # NumPy stores bools as one byte, matching rumi's padded binary samples.
    (_DL_UINT, 1): np.bool_,
    (_DL_UINT, 8): np.uint8,
    (_DL_INT, 8): np.int8,
    (_DL_UINT, 16): np.uint16,
    (_DL_INT, 16): np.int16,
    (_DL_UINT, 32): np.uint32,
    (_DL_INT, 32): np.int32,
    (_DL_UINT, 64): np.uint64,
    (_DL_INT, 64): np.int64,
    (_DL_FLOAT, 16): np.float16,
    (_DL_FLOAT, 32): np.float32,
    (_DL_FLOAT, 64): np.float64,
    (_DL_COMPLEX, 64): np.complex64,
    (_DL_COMPLEX, 128): np.complex128,
}

_ML_SCALARS: dict[tuple[int, int], str] = {
    (_DL_F8_E4M3FN, 8): "float8_e4m3fn",
    (_DL_F8_E5M2, 8): "float8_e5m2",
    (_DL_BFLOAT, 16): "bfloat16",
    (_DL_F8_E8M0, 8): "float8_e8m0fnu",
    (_DL_UINT, 4): "uint4",
    (_DL_INT, 4): "int4",
    (_DL_UINT, 2): "uint2",
    (_DL_INT, 2): "int2",
    (_DL_F6_E2M3, 6): "float6_e2m3fn",
    (_DL_F6_E3M2, 6): "float6_e3m2fn",
    (_DL_F4_E2M1, 4): "float4_e2m1fn",
}

_ml_dtypes: ModuleType | None
try:
    import ml_dtypes as _ml_dtypes
except ImportError:
    _ml_dtypes = None


@dataclass(frozen=True)
class _DType:
    name: str
    bits: int
    scalar: type | None
    ml_name: str | None = None


def _load_registry() -> dict[int, _DType]:
    out = ffi.new("const rumi_dtype_info**")
    count = lib.rumi_dtype_table(out)
    registry = {0: _DType("unknown", 0, None)}

    for i in range(count):
        row = out[0][i]
        key = int(row.dl_code), int(row.dl_bits)
        ml_name = _ML_SCALARS.get(key)
        scalar = _NUMPY_SCALARS.get(key)
        if scalar is None and ml_name is not None and _ml_dtypes is not None:
            scalar = getattr(_ml_dtypes, ml_name, None)

        registry[int(row.code)] = _DType(
            name=ffi.string(row.name).decode("ascii"),
            bits=int(row.bits),
            scalar=scalar,
            ml_name=ml_name,
        )

    return registry


_DTYPES = _load_registry()


def name(rumi_dtype: int) -> str:
    info = _DTYPES.get(rumi_dtype)
    return info.name if info is not None else f"rumi_dtype({rumi_dtype})"


def numpy_dtype(rumi_dtype: int) -> type:
    info = _DTYPES.get(rumi_dtype)
    if info is None:
        raise KeyError(rumi_dtype)
    if info.scalar is not None:
        return info.scalar
    if info.ml_name is not None:
        raise NotImplementedError(
            f"{info.name} needs ml_dtypes, pip install ml_dtypes")
    raise KeyError(rumi_dtype)


def dtype_code(dtype) -> int:
    resolved = np.dtype(dtype)
    for code, info in _DTYPES.items():
        if info.scalar is resolved.type:
            return code
    raise TypeError(f"dtype {resolved} is not supported by rumi")


def is_subbyte(rumi_dtype: int) -> bool:
    info = _DTYPES.get(rumi_dtype)
    return info is not None and 0 < info.bits < 8


def check_samples(arr, rumi_dtype: int) -> None:
    """Validate decoded samples using the core's dtype rules."""
    view = arr.reshape(-1).view("uint8")
    _check(lib.rumi_check_samples(ffi.from_buffer(view), view.nbytes,
                                  rumi_dtype))
