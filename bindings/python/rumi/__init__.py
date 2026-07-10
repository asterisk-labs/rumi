from importlib.metadata import version

from ._header import RumiHeader
from ._read import read
from ._write import probe, write

__version__ = version("rumi-eo")

__all__ = ["RumiHeader", "read", "write", "probe", "__version__"]