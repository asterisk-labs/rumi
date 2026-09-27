from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

import numpy as np

from ._dtype import DType, dtype_info


def _unsupported(framework: str, dtype: DType) -> TypeError:
    return TypeError(f"{framework} cannot represent rumi dtype {dtype.name}")


def _check_result(value: Any, dtype: DType, framework: str) -> None:
    actual = getattr(value.dtype, "name", str(value.dtype))
    actual = actual.removeprefix("torch.")
    if actual != dtype.name:
        raise TypeError(
            f"{framework} imported rumi dtype {dtype.name} as {actual}")


def _validate_numpy(dtype: DType) -> None:
    if dtype.numpy_dtype is None:
        raise _unsupported("NumPy", dtype)


def _to_numpy(array: Any, dtype: DType) -> Any:
    value = np.from_dlpack(array)
    if value.dtype.type is not dtype.numpy_dtype:
        raise TypeError(
            f"NumPy imported rumi dtype {dtype.name} as {value.dtype}")
    return value


def _validate_torch(_dtype: DType) -> None:
    import torch  # noqa: F401


def _to_torch(array: Any, dtype: DType) -> Any:
    import torch

    value = torch.from_dlpack(array)
    _check_result(value, dtype, "PyTorch")
    return value


_DL_INT = 0
_DL_UINT = 1
_DL_FLOAT = 2
_DL_BFLOAT = 4
_DL_COMPLEX = 5
_DL_BOOL = 6


def _dlpack_types(code: int, *bits: int) -> set[tuple[int, int, int]]:
    return {(code, width, 1) for width in bits}


_JAX_DLPACK = frozenset(
    _dlpack_types(_DL_UINT, 8, 16, 32)
    | _dlpack_types(_DL_INT, 8, 16, 32)
    | _dlpack_types(_DL_FLOAT, 16, 32)
    | _dlpack_types(_DL_COMPLEX, 64)
    | _dlpack_types(_DL_BFLOAT, 16)
    | _dlpack_types(_DL_BOOL, 8)
)
_JAX_X64_DLPACK = frozenset(
    _dlpack_types(_DL_UINT, 64)
    | _dlpack_types(_DL_INT, 64)
    | _dlpack_types(_DL_FLOAT, 64)
    | _dlpack_types(_DL_COMPLEX, 128)
)


def _validate_jax(dtype: DType) -> None:
    import jax

    if dtype.dlpack in _JAX_X64_DLPACK:
        if not jax.config.x64_enabled:
            raise TypeError(
                f"JAX cannot represent rumi dtype {dtype.name} while "
                "jax_enable_x64 is disabled")
    elif dtype.dlpack not in _JAX_DLPACK:
        raise _unsupported("JAX", dtype)


def _to_jax(array: Any, dtype: DType) -> Any:
    import jax

    value = jax.dlpack.from_dlpack(array)
    _check_result(value, dtype, "JAX")
    return value


_TENSORFLOW_DLPACK = _JAX_DLPACK | _JAX_X64_DLPACK


def _validate_tensorflow(dtype: DType) -> None:
    import tensorflow  # noqa: F401

    if dtype.dlpack not in _TENSORFLOW_DLPACK:
        raise _unsupported("TensorFlow", dtype)


def _to_tensorflow(array: Any, dtype: DType) -> Any:
    import tensorflow as tf

    value = tf.experimental.dlpack.from_dlpack(array.__dlpack__())
    _check_result(value, dtype, "TensorFlow")
    return value


def _to_dlpack(array: Any, _dtype: DType) -> Any:
    return array


def _accept(_dtype: DType) -> None:
    pass


@dataclass(frozen=True)
class _Consumer:
    dtype: DType
    transfer: Callable[[Any, DType], Any]

    def convert(self, array: Any) -> Any:
        return self.transfer(array, self.dtype)


_FRAMEWORKS = {
    "numpy": (_validate_numpy, _to_numpy),
    "torch": (_validate_torch, _to_torch),
    "jax": (_validate_jax, _to_jax),
    "tensorflow": (_validate_tensorflow, _to_tensorflow),
    "dlpack": (_accept, _to_dlpack),
}


def resolve_framework(name: str, dtype_code: int) -> _Consumer:
    try:
        validate, transfer = _FRAMEWORKS[name]
    except (KeyError, TypeError):
        choices = ", ".join(repr(value) for value in _FRAMEWORKS)
        raise ValueError(
            f"unknown framework {name!r}; expected {choices}") from None
    dtype = dtype_info(dtype_code)
    validate(dtype)
    return _Consumer(dtype, transfer)
