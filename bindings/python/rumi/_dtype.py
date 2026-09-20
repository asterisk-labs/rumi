import importlib
from dataclasses import dataclass
from types import ModuleType

import numpy as np

from ._ffi import _check, ffi, lib

_ml_dtypes: ModuleType | None
try:
    _ml_dtypes = importlib.import_module("ml_dtypes")
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
        # The registry names one scalar; NumPy owns it, or ml_dtypes does.
        named = (ffi.string(row.scalar).decode("ascii")
                 if row.scalar != ffi.NULL else None)
        scalar = getattr(np, named, None) if named is not None else None
        ml_name = named if named is not None and scalar is None else None
        if ml_name is not None and _ml_dtypes is not None:
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


def is_ml_float(rumi_dtype: int) -> bool:
    """Byte-wide ML floats, which NumPy cannot import through DLPack."""
    info = _DTYPES.get(rumi_dtype)
    return info is not None and info.ml_name is not None and info.bits >= 8


def check_samples(arr, rumi_dtype: int) -> None:
    """Validate decoded samples using the core's dtype rules."""
    view = arr.reshape(-1).view("uint8")
    _check(lib.rumi_check_samples(ffi.from_buffer(view), view.nbytes,
                                  rumi_dtype))
