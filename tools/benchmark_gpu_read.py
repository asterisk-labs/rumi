#!/usr/bin/env python3
"""Time from a GPU read call to a ready PyTorch tensor.

Every measured trial runs in a fresh Python process because rumi fixes the
size of its process-wide thread pool on first use. A trial reports the first
read of the process, warm and cold whole-scene reads, read_many batches with a
synchronization after each, and pipelined batches that each feed a kernel
without waiting, as a training loop does. Cold reads drop the page cache of the
file first, so they include the local disk. Run it once per build to compare
two builds on the same data.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import time
from pathlib import Path

import rumi

RECIPES = {"pfor": "planar>zigzag>pfor", "pivco": "planar>zigzag>lanes"}
METRICS = ("first_ms", "warm_ms", "cold_ms", "batch_ms", "pipelined_ms")


def synthetic(size: int, bands: int, seed: int):
    """Smooth uint16 bands with sensor-like noise."""
    import numpy as np

    rng = np.random.default_rng(seed)
    axis = np.linspace(0, 8 * np.pi, size, dtype=np.float32)
    out = np.empty((bands, size, size), dtype=np.uint16)
    for band in range(bands):
        phase = rng.uniform(0, np.pi, 2)
        field = np.add.outer(np.sin(axis + phase[0]), np.cos(axis * 0.7 + phase[1]))
        noisy = 2000 + 600 * field + rng.normal(0, 25, (size, size))
        out[band] = np.clip(noisy, 0, 65535).astype(np.uint16)
    return out


def prepare_data(args: argparse.Namespace) -> Path:
    """Encode the scene once per recipe, reusing an exact match."""
    directory = args.data_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    manifest = directory / "manifest.json"
    wanted = {
        "source": str(args.source.resolve()) if args.source else None,
        "size": args.size,
        "bands": args.bands,
        "tile": args.tile,
        "recipes": sorted(args.recipes),
        "seed": args.seed,
    }
    if manifest.exists():
        current = json.loads(manifest.read_text())
        if current.get("parameters") == wanted:
            return manifest

    import geozl

    if args.source:
        source = str(args.source)
        data = rumi.read(source, rumi.info(source=source).header)
        if data.ndim != 3:
            raise SystemExit("--source must be an Image of shape (bands, y, x)")
    else:
        data = synthetic(args.size, args.bands, args.seed)

    for name in args.recipes:
        frames = rumi.frames(
            data, "b (row h) (col w) -> row col b (h w)", tile_size=args.tile)
        graphs = {}
        for frame in frames:
            graph = graphs.get(frame.data.shape)
            if graph is None:
                graph = graphs[frame.data.shape] = geozl.graph(
                    frame.data, RECIPES[name])
            frame.compressed = geozl.compress(frame.data, graph=graph)
        _, header = rumi.write(
            directory / f"{name}.rumi", frames,
            bands=[f"Band {b + 1}" for b in range(data.shape[0])],
            time=["2024-01-01"])
        (directory / f"{name}.header").write_bytes(header)
    os.sync()
    manifest.write_text(json.dumps({
        "parameters": wanted, "shape": list(data.shape)}))
    return manifest


def drop_cache(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)


def child_trial(args: argparse.Namespace) -> None:
    import numpy as np
    import torch

    rumi.set_num_threads(args.threads)
    directory = args.manifest.parent
    shape = json.loads(args.manifest.read_text())["shape"]
    path = directory / f"{args.recipe}.rumi"
    header = (directory / f"{args.recipe}.header").read_bytes()
    device = torch.device(args.device)
    sync = torch.cuda.synchronize
    cold_reads = hasattr(os, "posix_fadvise")

    def scene() -> float:
        start = time.perf_counter()
        out = rumi.read(path, header, framework="torch", device=args.device)
        sync(device)
        elapsed = (time.perf_counter() - start) * 1000
        if list(out.shape) != shape:
            raise RuntimeError(f"read {list(out.shape)}, expected {shape}")
        return elapsed

    torch.zeros(1, device=device)
    sync(device)
    if cold_reads:
        drop_cache(path)
    first = scene()
    # GPU clocks are still rising after the first read.
    for _ in range(2):
        scene()
    warm = [scene() for _ in range(args.repeats)]
    cold = []
    if cold_reads:
        for _ in range(args.repeats):
            drop_cache(path)
            cold.append(scene())

    rng = np.random.default_rng(args.seed)
    high = [shape[1] - args.chip, shape[2] - args.chip]
    batches = [
        [(int(rng.integers(0, high[0] + 1)), int(rng.integers(0, high[1] + 1)),
          args.chip, args.chip) for _ in range(args.batch_size)]
        for _ in range(args.batches)
    ]

    def batch(windows):
        return rumi.read_many(
            [path] * len(windows), [header] * len(windows), windows=windows,
            framework="torch", device=args.device)

    for windows in batches[:10]:
        batch(windows)
    sync(device)
    latency = []
    for windows in batches:
        start = time.perf_counter()
        batch(windows)
        sync(device)
        latency.append((time.perf_counter() - start) * 1000)

    total = torch.zeros((), device=device, dtype=torch.float64)
    sync(device)
    start = time.perf_counter()
    for windows in batches:
        total += batch(windows).sum(dtype=torch.float64)
    sync(device)
    pipelined = (time.perf_counter() - start) * 1000 / len(batches)

    print(json.dumps({
        "recipe": args.recipe,
        "threads": args.threads,
        "first_ms": first,
        "warm_ms": statistics.median(warm),
        "cold_ms": statistics.median(cold) if cold else None,
        "batch_ms": statistics.median(latency),
        "pipelined_ms": pipelined,
    }))


def run_parent(args: argparse.Namespace, manifest: Path) -> None:
    configs = [(recipe, threads) for recipe in args.recipes
               for threads in args.threads_list]
    results = {config: [] for config in configs}
    order_rng = random.Random(args.seed)
    for trial in range(args.trials):
        jobs = list(configs)
        order_rng.shuffle(jobs)
        for recipe, threads in jobs:
            command = [
                sys.executable, str(Path(__file__).resolve()), "--child",
                "--manifest", str(manifest), "--recipe", recipe,
                "--threads", str(threads), "--device", args.device,
                "--chip", str(args.chip), "--batch-size", str(args.batch_size),
                "--batches", str(args.batches), "--repeats", str(args.repeats),
                "--seed", str(args.seed),
            ]
            try:
                completed = subprocess.run(
                    command, check=True, capture_output=True, text=True,
                    env=os.environ)
            except subprocess.CalledProcessError as error:
                sys.stderr.write(error.stdout)
                sys.stderr.write(error.stderr)
                raise
            record = json.loads(completed.stdout.strip().splitlines()[-1])
            results[recipe, threads].append(record)
            print(f"trial {trial + 1}/{args.trials} {recipe:6s} {threads:3d} "
                  f"threads  warm {record['warm_ms']:8.1f} ms  "
                  f"batch {record['batch_ms']:6.2f} ms", flush=True)

    print("\nMedian milliseconds over trials.\n")
    print("| recipe | threads | first | warm | cold | batch | pipelined |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for (recipe, threads), records in results.items():
        cells = []
        for metric in METRICS:
            values = [r[metric] for r in records if r[metric] is not None]
            cells.append(f"{statistics.median(values):.1f}" if values else "n/a")
        print(f"| {recipe} | {threads} | " + " | ".join(cells) + " |")


def parser() -> argparse.ArgumentParser:
    out = argparse.ArgumentParser(description=__doc__)
    out.add_argument("--data-dir", type=Path, default=Path("/tmp/rumi-gpu-read"))
    out.add_argument("--source", type=Path,
                     help="Image to re-encode instead of a synthetic scene")
    out.add_argument("--size", type=int, default=4096)
    out.add_argument("--bands", type=int, default=3)
    out.add_argument("--tile", type=int, default=512)
    out.add_argument("--recipes", nargs="+", choices=RECIPES,
                     default=list(RECIPES))
    out.add_argument("--threads", dest="threads_list", type=int, nargs="+",
                     default=[1, 8])
    out.add_argument("--device", default="cuda:0")
    out.add_argument("--chip", type=int, default=256)
    out.add_argument("--batch-size", type=int, default=32)
    out.add_argument("--batches", type=int, default=200)
    out.add_argument("--repeats", type=int, default=10)
    out.add_argument("--trials", type=int, default=3)
    out.add_argument("--seed", type=int, default=20261112)
    out.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    out.add_argument("--manifest", type=Path, help=argparse.SUPPRESS)
    out.add_argument("--recipe", choices=RECIPES, help=argparse.SUPPRESS)
    return out


def main() -> None:
    args = parser().parse_args()
    if args.child:
        args.threads = args.threads_list[0]
        child_trial(args)
        return
    manifest = prepare_data(args)
    run_parent(args, manifest)


if __name__ == "__main__":
    main()
