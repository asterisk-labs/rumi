from importlib.metadata import version

from ._config import (
    get_checksum_verification,
    get_num_threads,
    set_checksum_verification,
    set_num_threads,
)
from ._dlpack import RumiArray
from ._dtype import DType
from ._frames import Frame, FrameTable, frames
from ._info import Metadata, info
from ._read import read, read_many
from ._write import header_size, write

__version__ = version("rumi-eo")

__all__ = ["DType", "Frame", "FrameTable", "Metadata", "RumiArray", "frames",
           "get_checksum_verification", "get_num_threads", "header_size", "info",
           "read", "read_many", "set_checksum_verification", "set_num_threads",
           "write", "__version__"]
