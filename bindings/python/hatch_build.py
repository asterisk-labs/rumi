from pathlib import Path
from typing import Any

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

_LIB_GLOBS = ("*.so", "*.so.*", "*.dylib", "*.dll")

_NOTICE_FILES = (
    "LICENSE",
    "NOTICE",
    "licenses/LICENSE.GeoZL",
    "licenses/LICENSE.OpenZL",
    "licenses/LICENSE.Zstandard",
    "licenses/LICENSE.LZ4",
)


class CustomBuildHook(BuildHookInterface):
    def initialize(self, version: str, build_data: dict[str, Any]) -> None:
        lib_dir = Path(self.root) / "rumi" / "_lib"
        found = [p for g in _LIB_GLOBS for p in lib_dir.glob(g)]
        if not found:
            raise RuntimeError(
                f"no native library in {lib_dir}. Build it first: "
                "`make lib` from the repo root."
            )

        # Binary inside, so tag the wheel for this platform, not py3-none-any.
        build_data["pure_python"] = False
        build_data["infer_tag"] = True

        build_data.setdefault("force_include", {}).update(self._notices())

    def _notices(self) -> dict[str, str]:
        repo = Path(self.root).parent.parent
        sources = [repo / name for name in _NOTICE_FILES]
        missing = [str(path) for path in sources if not path.is_file()]
        if missing:
            raise RuntimeError(f"missing wheel notices: {', '.join(missing)}")

        # Wheel distribution names replace punctuation with underscores.
        dist_name = self.metadata.name.replace("-", "_").replace(".", "_")
        dist_info = f"{dist_name}-{self.metadata.version}.dist-info"
        return {
            str(source): f"{dist_info}/licenses/{source.name}"
            for source in sources
        }
