from importlib.metadata import version

from ._ffi import lib
from ._read import RumiArray, index_file, parse, read
from ._header import RumiHeader
from ._write import Layout, header_bytes, tile, write

__version__ = version("rumi-eo")

# The OpenZL frame format version.
OPENZL_VERSION = lib.rumi_openzl_format_version()

__all__ = [
    "Layout", "OPENZL_VERSION", "RumiArray", "RumiHeader", "__version__",
    "header_bytes", "index_file", "parse", "read", "tile", "write",
]
