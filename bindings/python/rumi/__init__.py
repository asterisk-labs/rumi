from importlib.metadata import version

from ._frames import Frame, FrameTable, frames
from ._header import RumiHeader
from ._read import read
from ._threads import get_num_threads, set_num_threads
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["Frame", "FrameTable", "RumiHeader", "frames", "get_num_threads",
           "read", "set_num_threads", "write", "__version__"]
