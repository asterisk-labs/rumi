"""rumi, a GeoTIFF profile for AI training data."""

from importlib.metadata import version

from ._read import index_file, parse, read
from ._spec import Spec
from ._write import Layout, tile, write

__version__ = version("rumi")

__all__ = [
    "Layout", "Spec", "__version__",
    "index_file", "parse", "read", "tile", "write",
]