"""The process-wide thread count, and the pool behind it.

Every case here turns on state that lives as long as the process, so each one
runs in its own interpreter. The fork cases are the regressions that matter:
they used to hang, and a hang is silent.
"""

import os
import subprocess
import sys
import textwrap

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

GRAPH = "planar>zigzag>zstd"

needs_fork = pytest.mark.skipif(not hasattr(os, "fork"),
                                reason="no fork on this platform")


@pytest.fixture(scope="module")
def image(tmp_path_factory):
    """Enough tiles that a parallel read has something to spread."""
    rng = np.random.default_rng(0)
    data = rng.integers(0, 3000, (2, 160, 160)).astype(np.uint16)
    tf = rumi.frames(data, 32)
    graphs = {}
    for t in tf:
        g = graphs.get(t.data.shape)
        if g is None:
            g = graphs[t.data.shape] = geozl.graph(t.data, GRAPH)
        t.compressed = geozl.compress(t.data, graph=g)
    path = tmp_path_factory.mktemp("threads") / "img.rumi"
    path, _ = rumi.write(path, tf)
    return str(path)


def run(image, body, **env):
    """Run body in a fresh interpreter and hand back what it printed."""
    src = textwrap.dedent(f"""
        import os, sys, time, warnings
        import rumi
        PATH = {image!r}
        HDR = rumi._read._header_of(PATH)
    """) + textwrap.dedent(body)
    # A deadlock would hang forever, so the snippets bound their own waits and
    # this timeout is only the backstop.
    child_env = {**os.environ, "RUMI_NUM_THREADS": "", **env}
    done = subprocess.run([sys.executable, "-c", src], capture_output=True,
                          text=True, timeout=180, env=child_env)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def test_default_is_one(image):
    assert run(image, "print(rumi.get_num_threads())",
               RUMI_NUM_THREADS="") == "1"


def test_the_environment_seeds_the_count(image):
    assert run(image, "print(rumi.get_num_threads())",
               RUMI_NUM_THREADS="3") == "3"


def test_an_invalid_environment_falls_back_to_one(image):
    assert run(image, "print(rumi.get_num_threads())",
               RUMI_NUM_THREADS="3 trailing") == "1"


def test_the_environment_is_bounded(image):
    assert run(image, "print(rumi.get_num_threads())",
               RUMI_NUM_THREADS="2000") == "1024"


def test_all_cpus_is_spelled_out(image):
    assert int(run(image, "print(rumi.get_num_threads())",
                   RUMI_NUM_THREADS="ALL_CPUS")) >= 1


def test_set_before_any_read(image):
    assert run(image, """
        print(rumi.set_num_threads(4), rumi.get_num_threads())
    """) == "4 4"


@pytest.mark.parametrize("bad", [0, -1, 1025, 1.5, "4"])
def test_python_rejects_an_invalid_count(bad):
    with pytest.raises((TypeError, ValueError)):
        rumi.set_num_threads(bad)


def test_a_serial_read_does_not_pin_the_count(image):
    """The torch.utils.benchmark.Timer bug: one read at 1 used to fix the size
    of the pool for the whole process."""
    assert run(image, """
        rumi.read(PATH, HDR, num_threads=1)
        print(rumi.set_num_threads(4), rumi.get_num_threads())
    """) == "4 4"


def test_a_default_serial_read_does_not_pin_the_count(image):
    assert run(image, """
        rumi.read(PATH, HDR)
        print(rumi.set_num_threads(4), rumi.get_num_threads())
    """) == "4 4"


def test_the_pool_pins_the_count_and_a_later_set_warns(image):
    assert run(image, """
        rumi.read(PATH, HDR, num_threads=4)
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            print(rumi.set_num_threads(2), len(caught))
    """) == "4 1"


def test_a_read_takes_the_process_count(image):
    assert run(image, """
        rumi.set_num_threads(4)
        rumi.read(PATH, HDR)
        print(rumi.get_num_threads())
    """) == "4"


def test_reads_agree_whatever_the_thread_count(image):
    assert run(image, """
        import numpy as np
        one = rumi.read(PATH, HDR, num_threads=1)
        many = rumi.read(PATH, HDR, num_threads=4)
        print(np.array_equal(one, many))
    """) == "True"


def test_concurrent_reads_share_the_pool_safely(image):
    assert run(image, """
        import concurrent.futures
        import numpy as np

        expected = rumi.read(PATH, HDR, num_threads=1)
        rumi.set_num_threads(4)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            reads = list(executor.map(
                lambda _: rumi.read(PATH, HDR), range(16)))
        print(all(np.array_equal(got, expected) for got in reads))
    """) == "True"


@needs_fork
def test_a_child_reads_after_a_parallel_parent(image):
    """fork() keeps only the calling thread, so the inherited pool has no
    workers. The child has to build its own instead of waiting forever."""
    assert run(image, """
        rumi.read(PATH, HDR, num_threads=4)
        sys.stdout.flush()
        pid = os.fork()
        if pid == 0:
            rumi.read(PATH, HDR, num_threads=4)
            os._exit(0)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                print("exit", os.waitstatus_to_exitcode(status))
                break
            time.sleep(0.05)
        else:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
            print("hung")
    """) == "exit 0"


@needs_fork
def test_a_child_defaults_to_one_after_parent_eda(image):
    """A DataLoader worker must not inherit the parent's notebook budget.
    PyTorch makes its own intra-op pool serial in every worker; rumi has to do
    the same independently because torch does not control third-party pools."""
    assert run(image, """
        rumi.read(PATH, HDR, num_threads=4)
        sys.stdout.flush()
        pid = os.fork()
        if pid == 0:
            ok = rumi.get_num_threads() == 1
            rumi.read(PATH, HDR)
            os._exit(0 if ok and rumi.get_num_threads() == 1 else 1)
        print("exit", os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]))
    """) == "exit 0"


@needs_fork
def test_a_child_reseeds_from_its_environment(image):
    assert run(image, """
        rumi.set_num_threads(4)
        os.environ["RUMI_NUM_THREADS"] = "3"
        sys.stdout.flush()
        pid = os.fork()
        if pid == 0:
            os._exit(0 if rumi.get_num_threads() == 3 else 1)
        print("exit", os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]))
    """) == "exit 0"


@needs_fork
def test_torch_dataloader_workers_stay_serial_after_parent_eda(image):
    pytest.importorskip("torch")
    assert run(image, """
        import torch

        rumi.read(PATH, HDR, num_threads=4)

        class Dataset(torch.utils.data.Dataset):
            def __len__(self):
                return 4

            def __getitem__(self, index):
                rumi.read(PATH, HDR, y=(0, 32), x=(0, 32))
                return rumi.get_num_threads(), torch.get_num_threads()

        loader = torch.utils.data.DataLoader(
            Dataset(), batch_size=None, num_workers=2,
            multiprocessing_context="fork")
        print(all(r == 1 and t == 1 for r, t in loader))
    """) == "True"


@needs_fork
def test_a_child_can_lower_the_count(image):
    """What worker_init_fn does. The parent's pool does not survive the fork,
    so nothing is pinned in the child and the request takes."""
    assert run(image, """
        rumi.read(PATH, HDR, num_threads=4)
        sys.stdout.flush()
        pid = os.fork()
        if pid == 0:
            os._exit(0 if rumi.set_num_threads(1) == 1 else 1)
        print("exit", os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]))
    """) == "exit 0"
