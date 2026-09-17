# Reading

Sources: `bindings/python/rumi/_read.py`, `core/src/read.cpp`, `core/src/plan.cpp`,
`core/src/source.cpp`, `core/src/capi.cpp`, `extern/karu/README.md`,
`extern/karu/CONFIGURATION.md`, `bindings/python/tests/test_many.py` and
`test_threads.py`. Examples ran against rumi 0.21.3, including the public fixtures.

## Contents

1. Sources
2. Cloud credentials and transport options
3. Selections
4. What a read fetches and decodes
5. Batches with `read_many`
6. PyTorch DataLoader
7. Frameworks and DLPack
8. Threads, concurrency and fork
9. Resource limits

## 1. Sources

`read`, `read_many` and `info` accept the same sources. Local paths and remote objects
go through Rumi's internal transport, Karu.

| Storage | URI | VSI path |
| --- | --- | --- |
| Local file | `scene.rumi`, `pathlib.Path` | |
| Bytes in memory | `bytes`, `bytearray`, `memoryview` | |
| HTTP(S) | `https://host/scene.rumi` | `/vsicurl/https://host/scene.rumi` |
| Amazon S3 | `s3://bucket/key` | `/vsis3/bucket/key` |
| Google Cloud Storage | `gs://bucket/key` | `/vsigs/bucket/key` |
| Azure Blob Storage | `az://container/key` | `/vsiaz/container/key` |
| Azure Data Lake | `abfs://container/key` | `/vsiadls/container/key` |
| Hugging Face | `hf://datasets/org/repo@revision/path` | `/vsihf/datasets/org/repo/path` |
| Source Cooperative | `source://account/product/key` | `/vsisource/account/product/key` |
| Byte range of any of these | | `/vsisubfile/OFFSET_LENGTH,<source>` |

- `/vsisubfile/1000_65714,container.bin` reads a Rumi file stored at byte 1000 of a
  larger object (a container, or an uncompressed archive member). It nests and wraps
  remote sources too.
- `/vsizip/`, `/vsigzip/` and `*_streaming` handlers are refused: `could not resolve
  /vsizip/x.zip/a.rumi: unsupported virtual filesystem '/vsizip/'; ...`.
- With a header, a remote read sends only range requests: no size, HEAD or listing
  request. `rumi.info(source=uri)` reads the file's structure first, so rebuild headers
  once, when building a catalog.

```python
from urllib.request import Request, urlopen

import rumi

base = "asterisk-labs/rumi-api-fixtures"
url = f"https://data.source.coop/{base}/headers/s2-00-tile.header"
header = urlopen(Request(url, headers={"User-Agent": "rumi-example"})).read()

chip = rumi.read(f"source://{base}/data/s2-00-tile.rumi", header,
                 bands=[3, 0], window=(237, 233, 91, 107))       # (2, 91, 107) uint16
same = rumi.read(f"hf://datasets/{base}@main/data/s2-00-tile.rumi", header,
                 bands=[3, 0], window=(237, 233, 91, 107))
```

The Source Cooperative download service answers 403 without a `User-Agent`. The same
object reads through `https://data.source.coop/...` and `/vsicurl/https://...`.

## 2. Cloud credentials and transport options

Rumi reads Karu's options from environment variables when each `read`, `read_many` or
`info` call starts, so a change applies to the next call. There are no per-source options
in the Python API. The full list is `extern/karu/CONFIGURATION.md`.

| Backend | Common variables |
| --- | --- |
| Amazon S3 | `AWS_PROFILE`, `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_SESSION_TOKEN`, `AWS_REGION`, `AWS_ENDPOINT_URL`, `AWS_NO_SIGN_REQUEST`, `AWS_VIRTUAL_HOSTING`, `AWS_REQUEST_PAYER`; web identity, ECS and EC2 metadata credentials are discovered |
| Google Cloud Storage | `GOOGLE_APPLICATION_CREDENTIALS`, `GCS_ACCESS_TOKEN`, `GCS_NO_SIGN_REQUEST`, `GCS_USER_PROJECT` |
| Azure | `AZURE_STORAGE_CONNECTION_STRING`, `AZURE_STORAGE_ACCOUNT` with `AZURE_STORAGE_ACCESS_KEY` or `AZURE_STORAGE_SAS_TOKEN`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`, `AZURE_TENANT_ID`, `AZURE_NO_SIGN_REQUEST` |
| Hugging Face | `HF_TOKEN`, `HF_TOKEN_PATH`, `HF_HOME`, `HF_ENDPOINT`; the token saved by `hf auth login` is found automatically |
| Source Cooperative | none for public data; `SOURCE_PROFILE`, `SOURCE_ACCESS_KEY_ID`, `SOURCE_SECRET_ACCESS_KEY`, `SOURCE_NO_SIGN_REQUEST` |
| HTTP | `KARU_HTTP_HEADERS`, `KARU_HTTP_CA_BUNDLE` (or `CURL_CA_BUNDLE`, `SSL_CERT_FILE`), `KARU_HTTP_PROXY` |
| Transport | `KARU_CONCURRENCY` (64), `KARU_COALESCE_GAP` (1 MiB), `KARU_MAX_ATTEMPTS` (3), `KARU_REQUEST_TIMEOUT` (120 s), `KARU_CONNECT_TIMEOUT` (30 s) |

An S3-compatible endpoint, here Source Cooperative's public one:

```python
import os

os.environ["AWS_ENDPOINT_URL"] = "https://data.source.coop"
os.environ["AWS_NO_SIGN_REQUEST"] = "YES"
os.environ["AWS_VIRTUAL_HOSTING"] = "NO"
chip = rumi.read("s3://asterisk-labs/rumi-api-fixtures/data/s2-00-tile.rumi", header,
                 bands=[3, 0], window=(237, 233, 91, 107))
```

Each calling thread keeps its connection pool while the configuration is unchanged.
Nothing else is cached: no object bytes, sizes or metadata survive a call.

## 3. Selections

| Argument | Accepts | Refuses |
| --- | --- | --- |
| `time`, `bands` | `None` (all, file order); a list of zero-based positions in any order, repeats allowed, NumPy integers included; a half-open `(start, stop)` tuple | NumPy arrays (`TypeError: bands: expected tuple or list, got ndarray`), empty lists (`ValueError: bands and n_bands must agree (both empty or both set)`), positions outside the axis |
| `window` | `None`, or a tuple `(row, column, height, width)` of integers inside the image | lists (`TypeError: window: expected (row, column, height, width) tuple`), negative origins, empty sizes, windows past the edge (`ValueError: window: requested window is out of image bounds`) |

- Windows are never padded. Clamp training crops so they stay inside each image.
- The selection sets each axis length; the file sets which axes exist. Default output
  patterns and `pattern=` are in `patterns.md` section 5.

## 4. What a read fetches and decodes

1. The window selects tile rows and columns.
2. The frame unit turns those positions and the selected bands and steps into frame
   indices: one frame per (band, step) for `h w` layouts, one per cell otherwise.
3. The header gives each frame's offset and size, and duplicate ranges are dropped.
4. Ranges are fetched (positional reads locally, range requests remotely; nearby remote
   ranges may merge into one request), each frame is decoded as its bytes arrive, and
   the requested samples are copied into the result.

- A cell frame is decoded once per grid position, whatever the band selection.
- A frame whose decoded order matches the output and lies inside the window decodes
  straight into the result, as with whole `b h w` frames and the default pattern.
- A decode failure cancels outstanding remote transfers. Items keep their order whatever
  order ranges complete in.

## 5. Batches with `read_many`

```python
batch = rumi.read_many(
    [path_a, "s3://bucket/b.rumi", data_bytes],
    [header_a, header_b, header_c],
    windows=[(0, 0, 256, 256), (400, 100, 256, 256), (128, 128, 256, 256)],
    bands=[0, 1, 2],
)                                                    # (3, 3, 256, 256)
```

- Windows share one height and width; each window is checked against its own image
  (`ValueError: item 2: requested window out of bounds`).
- Items must agree on tile size, band count, dtype, time count, and which of band and
  time a frame holds. `b h w` and `h w b` files batch together; `h w` and `b h w` do not
  (`ValueError: item 2: frame layout mismatch, 'h w' against 'b h w'`). Image extents
  may differ.
- One call plans every item together, so remote waits and decoding overlap across items.
  Prefer it to a Python loop over `read`.
- The same source may appear several times, for several windows of one scene.

## 6. PyTorch DataLoader

A dataset with `__getitems__` receives the whole index batch, so each batch is one
`read_many` call. This ran with `num_workers=0` and `num_workers=2` on macOS:

```python
import torch
import rumi


def identity(batch):
    return batch


class Windows(torch.utils.data.Dataset):
    def __init__(self, paths, headers, windows):
        self.paths, self.headers, self.windows = paths, headers, windows

    def __len__(self):
        return len(self.paths)

    def __getitem__(self, index):
        return self.__getitems__([index])[0]

    def __getitems__(self, indices):
        return rumi.read_many([self.paths[i] for i in indices],
                              [self.headers[i] for i in indices],
                              windows=[self.windows[i] for i in indices],
                              framework="torch")


loader = torch.utils.data.DataLoader(Windows(paths, headers, windows), batch_size=32,
                                     shuffle=True, collate_fn=identity, num_workers=4)
```

- Rumi keeps no open handles, so a dataset holds only paths, header bytes and windows,
  and pickles cheaply to spawned workers.
- Each worker process has its own pool: 1 thread, or `RUMI_NUM_THREADS`. Set another
  count in `worker_init_fn` with `rumi.set_num_threads(k)`.
- `collate_fn` must be a top-level function when workers are spawned.

## 7. Frameworks and DLPack

| `framework` | Result |
| --- | --- |
| `"numpy"` (default) | `numpy.ndarray` |
| `"torch"` | `torch.Tensor` on the CPU |
| `"jax"` | `jax.Array` |
| `"tensorflow"` or `"tf"` | `tf.Tensor` |
| `None` | `RumiArray`, exported later with `numpy()`, `torch()`, `jax()`, `tensorflow()` |

- Results decode on the CPU and move to the framework through DLPack without a copy.
  A `RumiArray` exports once; a second export raises `RuntimeError: this RumiArray was
  already exported`.
- Sub-byte and `bool` results are NumPy-backed and refuse DLPack (`BufferError: padded
  sub-byte dtypes cannot be exported through DLPack; use numpy()`), so
  `framework="torch"` fails for them. Read NumPy and convert.
- NumPy cannot import `float8_e4m3fn`, `float8_e5m2`, `float8_e8m0fnu` or `bfloat16`
  through DLPack, so NumPy reads view the decoded bytes as `ml_dtypes` arrays. Rumi
  0.21.3 raises `SystemError` there instead; on that release use `framework="torch"`.
- PyTorch's unsigned 16, 32 and 64-bit tensors support few operations; cast before
  training.
- An unknown name raises `ValueError: unknown framework 'cupy'`; a missing package raises
  its `ModuleNotFoundError` at read time.

## 8. Threads, concurrency and fork

- One process-wide pool decodes frames for every read. Configure it before the first
  parallel read (`python-api.md` section 8).
- `read`, `read_many` and `info` may run concurrently from several Python threads. The C
  call releases the GIL, and concurrent calls share the pool.
- After `fork`, the child starts with a fresh setting: 1, or its own `RUMI_NUM_THREADS`.
  It may call `set_num_threads` before its first parallel read.

## 9. Resource limits

- Decoded frames and other input-sized allocations are limited to 1 GiB per process by
  default. Raising it is a C call, `rumi_set_max_frame_bytes(n)` (`0` restores the
  default); from Python only through the private `rumi._ffi.lib`.
- Refusals: `that window reaches N frames, past the N bytes of ranges this reader will
  allocate` and `selecting every time step is past what this reader will allocate; pass
  the steps you want`.
- Headers whose frame sizes vary are expanded in memory, up to 64 MiB of index.
