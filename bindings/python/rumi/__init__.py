from importlib.metadata import version

from ._frames import Frame, FrameTable, frames
from ._info import Metadata, info
from ._read import read, read_many
from ._threads import get_num_threads, set_num_threads
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["Frame", "FrameTable", "Metadata", "frames", "get_num_threads",
           "info", "read", "read_many", "set_num_threads", "write",
           "__version__"]
