from importlib.metadata import version

from ._ffi import lib
from ._read import index_file, parse, read
from ._spec import Spec
from ._write import Layout, tile, write

__version__ = version("rumi")

# The OpenZL frame format version.
OPENZL_VERSION = lib.rumi_openzl_format_version()

__all__ = [
    "Layout", "OPENZL_VERSION", "Spec", "__version__",
    "index_file", "parse", "read", "tile", "write",
]