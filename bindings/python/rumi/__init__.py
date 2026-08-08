from importlib.metadata import version

from ._header import RumiHeader
from ._read import read
from ._tiles import Tile, TileFrame, tile
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["RumiHeader", "Tile", "TileFrame", "read", "tile", "write",
           "__version__"]
