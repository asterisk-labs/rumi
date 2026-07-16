from importlib.metadata import version

from ._header import RumiHeader
from ._read import read
from ._write import write

__version__ = version("rumi-eo")

__all__ = ["RumiHeader", "read", "write", "__version__"]