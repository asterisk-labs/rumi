#!/usr/bin/env python3
"""Run the Python, R and Julia examples on every docs page.

The blocks of one language on one page run in page order as one script, in a
fresh directory, so a block may use what an earlier block made. Signature
blocks and blocks marked data-run="skip" are left out. Any failure, or a
missing interpreter for a language the pages use, exits non-zero.

The notebook pages under docs/notebooks are rendered from executed notebooks
and need a GPU, so they are not run here.
"""

from __future__ import annotations

import argparse
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"

BLOCK = re.compile(
    r'<pre(?P<attrs>[^>]*)><code class="(?P<classes>[^"]*)">(?P<code>.*?)</code></pre>',
    re.DOTALL,
)
LANGUAGE = re.compile(r"\blanguage-(\w+)\b")
EXTENSIONS = {"python": "py", "r": "R", "julia": "jl"}
TAIL = 40


@dataclass
class Script:
    page: str
    language: str
    blocks: list[str]
    skipped: int

    @property
    def label(self) -> str:
        return f"{self.page} {self.language}"


@dataclass
class Result:
    script: Script
    ok: bool
    seconds: float
    output: str
    workdir: Path


def scripts(pages: list[Path], languages: list[str]) -> list[Script]:
    found = []
    for page in pages:
        text = page.read_text()
        for language in languages:
            blocks, skipped = [], 0
            for match in BLOCK.finditer(text):
                classes = match["classes"].split()
                named = LANGUAGE.search(match["classes"])
                if named is None or named[1] != language:
                    continue
                if "sig" in classes or 'data-run="skip"' in match["attrs"]:
                    skipped += 1
                    continue
                blocks.append(html.unescape(match["code"]))
            if blocks:
                found.append(Script(page.name, language, blocks, skipped))
    return found


def command(language: str, path: Path, julia_project: str | None) -> list[str]:
    if language == "python":
        return [sys.executable, str(path)]
    if language == "r":
        return ["Rscript", str(path)]
    project = [f"--project={julia_project}"] if julia_project else []
    return ["julia", "--startup-file=no", *project, str(path)]


def run(script: Script, args: argparse.Namespace, env: dict[str, str]) -> Result:
    workdir = Path(tempfile.mkdtemp(prefix=f"rumi-{Path(script.page).stem}-{script.language}-"))
    path = workdir / f"example.{EXTENSIONS[script.language]}"
    # Python, R and Julia all comment with #, so each block is marked for the
    # line numbers in a traceback.
    path.write_text("".join(f"# block {i + 1}\n{code}\n\n"
                            for i, code in enumerate(script.blocks)))
    started = time.monotonic()
    try:
        done = subprocess.run(command(script.language, path, args.julia_project),
                              cwd=workdir, env=env, capture_output=True, text=True, check=False,
                              timeout=args.timeout)
        ok, output = done.returncode == 0, done.stdout + done.stderr
    except subprocess.TimeoutExpired as exc:
        ok, output = False, f"timed out after {exc.timeout} s"
    except FileNotFoundError as exc:
        ok, output = False, f"{exc.filename} is not on PATH"
    seconds = time.monotonic() - started
    if ok and not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    return Result(script, ok, seconds, output, workdir)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pages", nargs="*", type=Path,
                        help="pages to run, every page under docs/ by default")
    parser.add_argument("--lang", action="append", choices=sorted(EXTENSIONS),
                        help="run only this language, may repeat")
    parser.add_argument("--julia-project", default=os.environ.get("JULIA_PROJECT"),
                        help="Julia environment with Rumi and ArchGDAL")
    parser.add_argument("--jobs", type=int, default=3, help="scripts run at once")
    parser.add_argument("--timeout", type=float, default=1800, help="seconds per script")
    parser.add_argument("--keep", action="store_true", help="keep the work directories")
    parser.add_argument("--list", action="store_true", help="list the scripts, run nothing")
    args = parser.parse_args()

    pages = [page.resolve() for page in args.pages] or sorted(DOCS.glob("*.html"))
    todo = scripts(pages, args.lang or sorted(EXTENSIONS))
    if args.list:
        for script in todo:
            print(f"{script.label}: {len(script.blocks)} blocks, {script.skipped} skipped")
        return 0
    if not todo:
        print("no examples to run")
        return 1

    env = dict(os.environ)
    # GDAL builds without the system certificates, such as Julia's, need them
    # named to read the public Sentinel-2 archive over HTTPS.
    bundles = [Path("/etc/ssl/cert.pem"), Path("/etc/ssl/certs/ca-certificates.crt")]
    bundle = next((path for path in bundles if path.is_file()), None)
    if "CURL_CA_BUNDLE" not in env and bundle is not None:
        env["CURL_CA_BUNDLE"] = str(bundle)

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda script: run(script, args, env), todo))

    failed = [result for result in results if not result.ok]
    for result in results:
        script = result.script
        print(f"{'PASS' if result.ok else 'FAIL'} {script.label}: {len(script.blocks)} blocks, "
              f"{script.skipped} skipped, {result.seconds:.0f} s")
    for result in failed:
        print(f"\n--- {result.script.label}, kept in {result.workdir}")
        print("\n".join(result.output.rstrip().splitlines()[-TAIL:]))
    print(f"\n{len(results) - len(failed)} of {len(results)} scripts passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
