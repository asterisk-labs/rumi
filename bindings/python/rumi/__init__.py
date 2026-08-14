from importlib.metadata import version

from ._header import RumiHeader
from ._read import read
from ._tiles import Frame, FrameTable, Tile, TileFrame, frames, tile
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["Frame", "FrameTable", "RumiHeader", "Tile", "TileFrame", "frames",
           "read", "tile", "write", "__version__"]
