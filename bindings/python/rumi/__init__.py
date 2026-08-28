from importlib.metadata import version

from ._chunks import chunks
from ._frames import Frame, FrameTable, frames
from ._geo import Geo, read_geo
from ._header import RumiHeader
from ._read import read
from ._threads import get_num_threads, set_num_threads
from ._time import Time
from ._time import read as read_time
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["Frame", "FrameTable", "Geo", "RumiHeader", "Time", "chunks", "frames",
           "get_num_threads", "read", "read_geo", "read_time",
           "set_num_threads", "write", "__version__"]
