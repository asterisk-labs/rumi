from importlib.metadata import version

from ._header import RumiHeader
from ._read import parse, read
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["RumiHeader", "parse", "read", "write", "__version__"]