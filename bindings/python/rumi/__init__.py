from importlib.metadata import version

from ._header import RumiHeader
from ._read import read
from ._tiles import Frame, FrameTable, frames
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["Frame", "FrameTable", "RumiHeader", "frames", "read", "write",
           "__version__"]
