function checkedRange(offset, length) {
  if (typeof offset !== "bigint" || offset < 0n) {
    throw new RangeError("offset must be a non-negative bigint");
  }
  if (!Number.isSafeInteger(length) || length < 0) {
    throw new RangeError("length must be a non-negative safe integer");
  }
}

function bytesFrom(value) {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer
      || (typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer)) {
    return new Uint8Array(value);
  }
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  return undefined;
}

class MemorySource {
  constructor(bytes) {
    this.bytes = bytes;
  }

  async read(offset, length) {
    checkedRange(offset, length);
    if (offset > BigInt(Number.MAX_SAFE_INTEGER)) throw new RangeError("memory offset is too large");
    const start = Number(offset);
    if (start > this.bytes.byteLength || length > this.bytes.byteLength - start) {
      throw new RangeError("read extends past the source");
    }
    return this.bytes.slice(start, start + length);
  }

  size() {
    return BigInt(this.bytes.byteLength);
  }
}

class BlobSource {
  constructor(blob) {
    this.blob = blob;
  }

  async read(offset, length) {
    checkedRange(offset, length);
    if (offset > BigInt(Number.MAX_SAFE_INTEGER)) throw new RangeError("Blob offset is too large");
    const start = Number(offset);
    if (start > this.blob.size || length > this.blob.size - start) {
      throw new RangeError("read extends past the Blob");
    }
    return new Uint8Array(await this.blob.slice(start, start + length).arrayBuffer());
  }

  size() {
    return BigInt(this.blob.size);
  }
}

class HttpSource {
  constructor(url, fetchImpl, signal) {
    this.url = url;
    this.fetch = (input, init) => fetchImpl.call(globalThis, input, init);
    this.signal = signal;
    this.total = undefined;
    this.etag = undefined;
  }

  async read(offset, length) {
    checkedRange(offset, length);
    if (length === 0) return new Uint8Array();
    const end = offset + BigInt(length) - 1n;
    const headers = { Range: `bytes=${offset}-${end}` };
    if (this.etag !== undefined && !this.etag.startsWith("W/")) {
      headers["If-Match"] = this.etag;
    }
    const response = await this.fetch(this.url, {
      headers,
      signal: this.signal,
    });
    if (response.status === 412) throw new Error("Rumi source changed while it was being read");
    if (response.status !== 206) {
      throw new Error(`server did not honor the byte range (HTTP ${response.status})`);
    }
    const contentRange = response.headers.get("Content-Range");
    const match = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(contentRange ?? "");
    if (!match || BigInt(match[1]) !== offset || BigInt(match[2]) !== end) {
      throw new Error("server returned the wrong Content-Range");
    }
    const total = BigInt(match[3]);
    if (total <= end) throw new Error("server returned an invalid Content-Range size");
    if (this.total !== undefined && this.total !== total) {
      throw new Error("Rumi source size changed while it was being read");
    }
    this.total = total;

    const etag = response.headers.get("ETag") ?? undefined;
    if (this.etag !== undefined && etag !== undefined && etag !== this.etag) {
      throw new Error("Rumi source changed while it was being read");
    }
    if (this.etag === undefined && etag !== undefined) this.etag = etag;
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (bytes.byteLength !== length) {
      throw new RangeError(`range returned ${bytes.byteLength} bytes, expected ${length}`);
    }
    return bytes;
  }

  size() {
    if (this.total === undefined) throw new Error("source size is not known yet");
    return this.total;
  }
}

export function sourceFrom(value, {
  fetch: fetchImpl = globalThis.fetch,
  signal,
} = {}) {
  const bytes = bytesFrom(value);
  if (bytes) return new MemorySource(bytes);
  if (typeof Blob !== "undefined" && value instanceof Blob) return new BlobSource(value);
  if (typeof value === "string" || value instanceof URL) {
    if (typeof fetchImpl !== "function") throw new Error("fetch is not available");
    return new HttpSource(value, fetchImpl, signal);
  }
  if (value && typeof value.read === "function" && typeof value.size === "function") return value;
  throw new TypeError("source must be bytes, a Blob, a URL, or an object with read() and size()");
}

export async function readExact(source, offset, length) {
  const bytes = await source.read(offset, length);
  if (!(bytes instanceof Uint8Array)) {
    throw new TypeError("source.read() must return a Uint8Array");
  }
  if (bytes.byteLength !== length) {
    throw new RangeError(`source returned ${bytes.byteLength} bytes, expected ${length}`);
  }
  return bytes;
}

export async function sourceSize(source) {
  const value = await source.size();
  if (typeof value !== "bigint" || value < 0n) {
    throw new TypeError("source.size() must return a non-negative bigint");
  }
  return value;
}
