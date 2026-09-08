#!/usr/bin/env python3
"""Reproducible local-throughput benchmark for rumi and PyTorch DataLoader.

Every measured trial runs in a fresh Python process because rumi fixes the
size of its process-wide thread pool on first use. The first epoch warms the
filesystem cache, decoder state, and persistent DataLoader workers; only the
second epoch is timed.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import random
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import rumi
import torch
from torch.utils.data import DataLoader, Dataset

CONFIGS = (
    "serial",
    "per-read",
    "python-threads",
    "read-many",
    "workers-4",
    "workers-8",
)


def identity(value):
    """Pickle-safe identity collator for an already batched tensor."""
    return value


def one_rumi_thread(_worker_id: int) -> None:
    """Keep each DataLoader process to one decoder thread."""
    torch.set_num_threads(1)
    rumi.set_num_threads(1)


class Samples(Dataset):
    def __init__(self, manifest: Path, window_mode: str) -> None:
        raw = json.loads(manifest.read_text())
        root = manifest.parent
        self.paths = [str(root / item["path"]) for item in raw["files"]]
        self.headers = [base64.b64decode(item["header"]) for item in raw["files"]]
        self.samples = raw["samples"][window_mode]

    def __len__(self) -> int:
        return len(self.samples)

    def _args(self, index: int):
        file_index, row, col, height, width = self.samples[index]
        return (
            self.paths[file_index],
            self.headers[file_index],
            (row, col, height, width),
        )

    def __getitem__(self, index: int):
        path, header, window = self._args(index)
        return rumi.read(path, header, window=window, framework="torch")


class ThreadedSamples(Samples):
    def __init__(self, manifest: Path, window_mode: str, threads: int) -> None:
        super().__init__(manifest, window_mode)
        self.executor = ThreadPoolExecutor(threads)

    def __getitems__(self, indices: list[int]):
        return list(self.executor.map(self.__getitem__, indices))


class BatchedSamples(Samples):
    def __getitems__(self, indices: list[int]):
        args = [self._args(index) for index in indices]
        return rumi.read_many(
            [item[0] for item in args],
            [item[1] for item in args],
            windows=[item[2] for item in args],
            framework="torch",
        )


def prepare_data(args: argparse.Namespace) -> Path:
    """Create deterministic fixtures and a manifest, reusing an exact match."""
    directory = args.data_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    manifest = directory / "manifest.json"
    wanted = {
        "scene_count": args.scenes,
        "scene_size": args.scene_size,
        "bands": args.bands,
        "tile": args.tile,
        "chip": args.chip,
        "sample_count": args.samples,
        "seed": args.seed,
    }
    if manifest.exists():
        current = json.loads(manifest.read_text())
        if current.get("parameters") == wanted:
            return manifest

    import geozl
    import numpy as np

    rng = np.random.default_rng(args.seed)
    pattern = "b (row h) (col w) -> row col (b h w)"
    graphs = {}
    files = []
    for index in range(args.scenes):
        data = rng.integers(
            0, 4096, (args.bands, args.scene_size, args.scene_size), dtype=np.uint16
        )
        frames = rumi.frames(data, pattern, args.tile)
        for frame in frames:
            graph = graphs.get(frame.data.shape)
            if graph is None:
                graph = graphs[frame.data.shape] = geozl.graph(
                    frame.data, "planar>zigzag>zstd"
                )
            frame.compressed = geozl.compress(frame.data, graph=graph)
        name = f"scene-{index:03d}.rumi"
        path, header = rumi.write(directory / name, frames)
        files.append({
            "path": Path(path).name,
            "header": base64.b64encode(header).decode("ascii"),
        })

    maximum = args.scene_size - args.chip
    aligned = []
    random_windows = []
    choices = list(range(0, maximum + 1, args.chip))
    for index in range(args.samples):
        file_index = index % args.scenes
        row = choices[(index // args.scenes) % len(choices)]
        col = choices[(index // (args.scenes * len(choices))) % len(choices)]
        aligned.append([file_index, row, col, args.chip, args.chip])
        random_windows.append([
            file_index,
            int(rng.integers(0, maximum + 1)),
            int(rng.integers(0, maximum + 1)),
            args.chip,
            args.chip,
        ])

    manifest.write_text(json.dumps({
        "parameters": wanted,
        "files": files,
        "samples": {"aligned": aligned, "random": random_windows},
    }))
    return manifest


def loader_for(
    config: str, manifest: Path, window_mode: str, batch_size: int, threads: int
):
    torch.set_num_threads(1)
    try:
        torch.set_num_interop_threads(1)
    except RuntimeError:
        pass

    if config == "python-threads":
        rumi.set_num_threads(1)
        dataset = ThreadedSamples(manifest, window_mode, threads)
        return DataLoader(dataset, batch_size=batch_size, num_workers=0)
    if config == "read-many":
        rumi.set_num_threads(threads)
        dataset = BatchedSamples(manifest, window_mode)
        return DataLoader(
            dataset, batch_size=batch_size, num_workers=0, collate_fn=identity
        )
    if config in {"workers-4", "workers-8"}:
        workers = int(config.rsplit("-", 1)[1])
        dataset = Samples(manifest, window_mode)
        return DataLoader(
            dataset,
            batch_size=batch_size,
            num_workers=workers,
            persistent_workers=True,
            multiprocessing_context="spawn",
            worker_init_fn=one_rumi_thread,
        )

    rumi.set_num_threads(1 if config == "serial" else threads)
    return DataLoader(
        Samples(manifest, window_mode), batch_size=batch_size, num_workers=0
    )


def consume(loader: DataLoader) -> tuple[int, int]:
    count = 0
    checksum = 0
    for batch in loader:
        count += int(batch.shape[0])
        checksum += int(batch[0, 0, 0, 0])
    return count, checksum


def child_trial(args: argparse.Namespace) -> None:
    window_mode = args.windows[0]
    loader = loader_for(
        args.config, args.manifest, window_mode, args.batch_size, args.threads
    )
    warm_count, warm_checksum = consume(loader)
    start = time.perf_counter()
    count, checksum = consume(loader)
    elapsed = time.perf_counter() - start
    if (count, checksum) != (warm_count, warm_checksum):
        raise RuntimeError("warm and measured epochs produced different results")
    print(json.dumps({
        "config": args.config,
        "windows": window_mode,
        "samples": count,
        "seconds": elapsed,
        "samples_per_second": count / elapsed,
        "checksum": checksum,
    }))


def run_parent(args: argparse.Namespace, manifest: Path) -> None:
    results = {mode: {config: [] for config in args.configs} for mode in args.windows}
    order_rng = random.Random(args.seed)
    for trial in range(args.trials):
        jobs = [(mode, config) for mode in args.windows for config in args.configs]
        order_rng.shuffle(jobs)
        for mode, config in jobs:
            command = [
                sys.executable,
                str(Path(__file__).resolve()),
                "--child",
                "--manifest",
                str(manifest),
                "--config",
                config,
                "--windows",
                mode,
                "--batch-size",
                str(args.batch_size),
                "--threads",
                str(args.threads),
            ]
            try:
                completed = subprocess.run(
                    command, check=True, capture_output=True, text=True, env=os.environ
                )
            except subprocess.CalledProcessError as error:
                sys.stderr.write(error.stdout)
                sys.stderr.write(error.stderr)
                raise
            record = json.loads(completed.stdout.strip().splitlines()[-1])
            results[mode][config].append(record["samples_per_second"])
            print(
                f"trial {trial + 1}/{args.trials} {mode:7s} {config:17s} "
                f"{record['samples_per_second']:9.1f} samples/s",
                flush=True,
            )

    print("\n| windows | configuration | median samples/s | range | speedup |")
    print("|---|---:|---:|---:|---:|")
    for mode in args.windows:
        baseline_name = "serial" if "serial" in args.configs else args.configs[0]
        baseline = statistics.median(results[mode][baseline_name])
        for config in args.configs:
            values = results[mode][config]
            median = statistics.median(values)
            label = (
                f"{config}-{args.threads}"
                if config in {"per-read", "python-threads", "read-many"}
                else f"{config}-1" if config == "serial" else config
            )
            print(
                f"| {mode} | {label} | {median:.1f} | "
                f"{min(values):.1f}-{max(values):.1f} | {median / baseline:.2f}x |"
            )


def parser() -> argparse.ArgumentParser:
    out = argparse.ArgumentParser(description=__doc__)
    out.add_argument("--data-dir", type=Path, default=Path("/tmp/rumi-dataloader"))
    out.add_argument("--scenes", type=int, default=24)
    out.add_argument("--scene-size", type=int, default=512)
    out.add_argument("--bands", type=int, default=16)
    out.add_argument("--tile", type=int, default=256)
    out.add_argument("--chip", type=int, default=256)
    out.add_argument("--samples", type=int, default=512)
    out.add_argument("--batch-size", type=int, default=64)
    out.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    out.add_argument("--seed", type=int, default=20260907)
    out.add_argument("--trials", type=int, default=5)
    out.add_argument("--configs", nargs="+", choices=CONFIGS, default=list(CONFIGS))
    out.add_argument(
        "--windows", nargs="+", choices=("aligned", "random"), default=["random"]
    )
    out.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    out.add_argument("--manifest", type=Path, help=argparse.SUPPRESS)
    out.add_argument("--config", choices=CONFIGS, help=argparse.SUPPRESS)
    return out


def main() -> None:
    args = parser().parse_args()
    if args.child:
        child_trial(args)
        return
    manifest = prepare_data(args)
    run_parent(args, manifest)


if __name__ == "__main__":
    main()
