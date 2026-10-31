import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { openRumi } from "../index.js";
import { expectedValues, makeFileRumi, makeRumi } from "./fixture.js";

const identityDecoder = (frame) => frame;
const entryValue = (index) => 24 + index * 20 + 12;

for (const frameUnit of [0, 3, 4, 5, 6, 7, 8, 9]) {
  test(`reads reordered cube selections from frame unit ${frameUnit}`, async () => {
    const fixture = makeFileRumi({ frameUnit });
    const dataset = await openRumi(fixture.file, { decoder: identityDecoder });
    const options = {
      time: [1, 0, 1],
      bands: [2, 0],
      window: [1, 1, 2, 3],
    };
    const result = await dataset.read(options);
    assert.deepEqual(result.shape, [3, 2, 2, 3]);
    assert.deepEqual(result.axes, ["time", "band", "y", "x"]);
    assert.deepEqual([...result.data], expectedValues({
      times: options.time,
      bands: options.bands,
      window: options.window,
    }));
  });
}

for (const frameUnit of [0, 1, 2]) {
  test(`reads an image from frame unit ${frameUnit}`, async () => {
    const fixture = makeFileRumi({ times: 1, frameUnit });
    const dataset = await openRumi(new Blob([fixture.file]), { decoder: identityDecoder });
    const result = await dataset.read({ bands: [2, 1], window: [0, 1, 3, 2] });
    assert.deepEqual(result.shape, [2, 3, 2]);
    assert.deepEqual(result.axes, ["band", "y", "x"]);
    assert.deepEqual([...result.data], expectedValues({
      times: [0],
      bands: [2, 1],
      window: [0, 1, 3, 2],
    }));
  });
}

for (const frameUnit of [1, 2]) {
  test(`reads a time-only cube from frame unit ${frameUnit}`, async () => {
    const fixture = makeFileRumi({ bands: 1, times: 2, frameUnit });
    const dataset = await openRumi(fixture.file, { decoder: identityDecoder });
    const result = await dataset.read({ time: [1, 0], window: [1, 0, 2, 4] });
    assert.deepEqual(result.shape, [2, 1, 2, 4]);
    assert.deepEqual([...result.data], expectedValues({
      times: [1, 0],
      bands: [0],
      window: [1, 0, 2, 4],
    }));
  });
}

test("plans unique ranges in file order", async () => {
  const fixture = makeFileRumi({ frameUnit: 0 });
  const dataset = await openRumi(fixture.file, { decoder: identityDecoder });
  const ranges = dataset.planRanges({
    time: [1, 1],
    bands: [2, 2],
    window: [0, 0, 2, 3],
  });
  assert.equal(ranges.length, 1);
  assert.equal(ranges[0].frameIndex, 5);
  assert.equal(ranges[0].length, fixture.frames[5].byteLength);
});

test("rebuilds the external header from a Rumi file", async () => {
  const transform = [0.01, 0, -77.1, 0, -0.01, -12.0];
  const fixture = makeFileRumi({ frameUnit: 8, transform, epsg: 4326 });
  const dataset = await openRumi(fixture.file, { decoder: identityDecoder });
  const options = { time: [1, 0], bands: [2, 0], window: [1, 1, 2, 3] };
  const result = await dataset.read(options);
  assert.equal(dataset.info().frameLayout, "h w t b");
  assert.equal(dataset.info().crs, "EPSG:4326");
  assert.deepEqual(dataset.info().transform, transform);
  assert.deepEqual(dataset.info().bands, fixture.bandTexts);
  assert.equal(dataset.info().timeKind, "instant");
  assert.deepEqual(dataset.info().time, fixture.timeCoordinates);
  assert.deepEqual([...result.data], expectedValues({
    times: options.time,
    bands: options.bands,
    window: options.window,
  }));
});

test("reads interval coordinates from the trailer", async () => {
  const timeCoordinates = [1700000001n, 1700003601n, 1700086401n, 1700090001n];
  const fixture = makeFileRumi({
    times: 2,
    timeKind: "interval",
    timeCoordinates,
  });
  const info = (await openRumi(fixture.file, { decoder: identityDecoder })).info();
  assert.equal(info.timeKind, "interval");
  assert.deepEqual(info.time, [
    [timeCoordinates[0], timeCoordinates[1]],
    [timeCoordinates[2], timeCoordinates[3]],
  ]);
});

test("rejects duplicate band descriptions in the trailer", async () => {
  const fixture = makeFileRumi({ bandTexts: ["same", "same", "other"] });
  await assert.rejects(
    () => openRumi(fixture.file, { decoder: identityDecoder }),
    /description is not unique/,
  );
});

test("rejects a file whose frame table does not match its layout", async () => {
  const fixture = makeFileRumi({ frameUnit: 0 });
  const broken = fixture.file.slice();
  const view = new DataView(broken.buffer);
  view.setUint32(24 + 12 * 20 + 12, 3, true);
  await assert.rejects(
    () => openRumi(broken, { decoder: identityDecoder }),
    /frame table.*expected/,
  );
});

test("uses strict HTTP range requests", async () => {
  const fixture = makeFileRumi({ frameUnit: 3 });
  const requests = [];
  async function fetch(_url, options) {
    assert.equal(this, globalThis);
    if (requests.length !== 0) assert.equal(options.headers["If-Match"], '"fixture-v1"');
    const match = /^bytes=(\d+)-(\d+)$/.exec(options.headers.Range);
    assert.ok(match);
    const start = Number(match[1]);
    const end = Number(match[2]);
    requests.push([start, end]);
    return new Response(fixture.file.slice(start, end + 1), {
      status: 206,
      headers: {
        "Content-Range": `bytes ${start}-${end}/${fixture.file.byteLength}`,
        ETag: '"fixture-v1"',
      },
    });
  }
  const dataset = await openRumi("https://example.test/a.rumi", {
    decoder: identityDecoder,
    fetch,
  });
  const result = await dataset.read({ time: [0], bands: [1], window: [0, 0, 1, 1] });
  assert.deepEqual([...result.data], [100]);
  assert.ok(requests.length >= 2);
  assert.deepEqual(requests[0], [0, 291]);
});

test("rejects a wrong HTTP Content-Range", async () => {
  const fixture = makeFileRumi();
  async function fetch() {
    return new Response(fixture.file.slice(0, 292), {
      status: 206,
      headers: { "Content-Range": `bytes 1-292/${fixture.file.byteLength}` },
    });
  }
  await assert.rejects(
    () => openRumi("https://example.test/a.rumi", { decoder: identityDecoder, fetch }),
    /wrong Content-Range/,
  );
});

test("opens metadata in three contiguous reads", async () => {
  const fixture = makeFileRumi({ bands: 13 });
  let reads = 0;
  const source = {
    size: () => BigInt(fixture.file.byteLength),
    async read(offset, length) {
      reads += 1;
      const start = Number(offset);
      return fixture.file.slice(start, start + length);
    },
  };
  await openRumi(source, { decoder: identityDecoder });
  assert.equal(reads, 3);
});

test("limits parallel frame reads", async () => {
  const fixture = makeFileRumi({
    width: 16,
    height: 2,
    times: 1,
    bands: 1,
    tileWidth: 2,
    tileHeight: 2,
    frameUnit: 0,
  });
  let active = 0;
  let busiest = 0;
  let track = false;
  const source = {
    size: () => BigInt(fixture.file.byteLength),
    async read(offset, length) {
      if (track) {
        active += 1;
        busiest = Math.max(busiest, active);
        await new Promise((resolve) => setTimeout(resolve, 5));
        active -= 1;
      }
      const start = Number(offset);
      return fixture.file.slice(start, start + length);
    },
  };
  const dataset = await openRumi(source, { decoder: identityDecoder });
  track = true;
  await dataset.read();
  assert.equal(busiest, 8);
});

test("stops scheduling frames after a read fails", async () => {
  const fixture = makeFileRumi({
    width: 512,
    height: 1,
    times: 1,
    bands: 1,
    tileWidth: 2,
    tileHeight: 1,
    frameUnit: 0,
  });
  let reads = 0;
  let decodes = 0;
  let track = false;
  const source = {
    size: () => BigInt(fixture.file.byteLength),
    async read(offset, length) {
      if (track) {
        reads += 1;
        await new Promise((resolve) => setTimeout(resolve, 1));
      }
      const start = Number(offset);
      return fixture.file.slice(start, start + length);
    },
  };
  const dataset = await openRumi(source, {
    decoder(frame) {
      decodes += 1;
      if (decodes === 1) throw new Error("decode failed");
      return frame;
    },
  });
  track = true;
  await assert.rejects(() => dataset.read(), /decode failed/);
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.ok(reads <= 8, `${reads} frames were read after the failure`);
  assert.ok(decodes <= 8, `${decodes} frames were decoded after the failure`);
});

test("accepts only complete Rumi files", async () => {
  const fixture = makeRumi();
  await assert.rejects(
    () => openRumi(fixture.file, { decoder: identityDecoder }),
    /source is not a Rumi file/,
  );
});

test("rejects sample fields that do not fit the external header", async () => {
  for (const [entry, value] of [[2, 272], [8, 257]]) {
    const fixture = makeFileRumi({ bands: 1, times: 1, frameUnit: 0 });
    const broken = fixture.file.slice();
    new DataView(broken.buffer).setUint16(entryValue(entry), value, true);
    await assert.rejects(
      () => openRumi(broken, { decoder: identityDecoder }),
      /unsupported sample format/,
    );
  }
});

test("rejects non-canonical trailer bytes", async () => {
  const fixture = makeFileRumi({ times: 3, timeCoordinates: [0n, 10n, 21n] });
  const padded = fixture.file.slice();
  padded[padded.length - 1] |= 0x80;
  await assert.rejects(
    () => openRumi(padded, { decoder: identityDecoder }),
    /padding is not zero/,
  );

  const trailing = new Uint8Array(fixture.file.byteLength + 1);
  trailing.set(fixture.file);
  await assert.rejects(
    () => openRumi(trailing, { decoder: identityDecoder }),
    /bytes after its trailer|time_bits is not canonical/,
  );
});

test("limits time metadata before allocating it", async () => {
  const fixture = makeFileRumi({
    width: 1,
    height: 1,
    times: 2,
    bands: 1,
    tileWidth: 1,
    tileHeight: 1,
    frameUnit: 1,
  });
  const broken = fixture.file.slice();
  new DataView(broken.buffer).setUint32(entryValue(12), 1_000_001, true);
  await assert.rejects(
    () => openRumi(broken, { decoder: identityDecoder }),
    /maxTimeCoordinates/,
  );
});

test("rejects time values whose POSIX seconds overflow int64", async () => {
  const max = (1n << 63n) - 1n;
  const fixture = makeFileRumi({
    times: 1,
    bands: 1,
    frameUnit: 0,
    timeCoordinates: [max * 86400n],
  });
  await assert.rejects(
    () => openRumi(fixture.file, { decoder: identityDecoder }),
    /seconds overflows int64/,
  );
});

test("preserves a leading BOM in a band description", async () => {
  const names = [`${String.fromCodePoint(0xfeff)}B4`, "B4", "B3"];
  const fixture = makeFileRumi({ bandTexts: names });
  const dataset = await openRumi(fixture.file, { decoder: identityDecoder });
  assert.deepEqual(dataset.info().bands, names);
});

test("requires the canonical transform for an undefined CRS", async () => {
  const fixture = makeFileRumi({ transform: [2, 0, 9, 0, -2, 7] });
  await assert.rejects(
    () => openRumi(fixture.file, { decoder: identityDecoder }),
    /identity transform/,
  );
});

test("validates EPSG codes and their model type", async () => {
  for (const epsg of [1025, 32718]) {
    const fixture = makeFileRumi({ epsg: 4326 });
    const file = fixture.file.slice();
    const view = new DataView(file.buffer);
    const directory = Number(view.getBigUint64(entryValue(10), true));
    view.setUint16(directory + 30, epsg, true);
    await assert.rejects(
      () => openRumi(file, { decoder: identityDecoder }),
      /not a supported|disagrees/,
    );
  }
});

test("rejects invalid decoded boolean bytes", async () => {
  const fixture = makeFileRumi({ bands: 1, times: 1, frameUnit: 0 });
  const file = fixture.file.slice();
  new DataView(file.buffer).setUint16(entryValue(2), 1, true);
  const dataset = await openRumi(file, {
    decoder: (_frame, { maxOutputSize }) => new Uint8Array(maxOutputSize).fill(2),
  });
  await assert.rejects(() => dataset.read(), /neither 0 nor 1/);
});

test("does not duplicate the absent time axis of an Image", async () => {
  const fixture = makeFileRumi({ times: 1, frameUnit: 0 });
  const dataset = await openRumi(fixture.file, { decoder: identityDecoder });
  await assert.rejects(() => dataset.read({ time: [0, 0] }), /one index/);
});

test("decodes a Rumi fixture with the GeoZL npm package", async () => {
  const bytes = await readFile(new URL("../../r/tests/testthat/fixtures/binding.rumi", import.meta.url));
  const dataset = await openRumi(bytes);
  const result = await dataset.read({ time: [0], bands: [0], window: [0, 0, 2, 3] });
  assert.deepEqual(result.shape, [1, 1, 2, 3]);
  assert.deepEqual([...result.data], [0, 1, 2, 100, 101, 102]);
});

test("rejects wrong decoder output and oversized reads", async () => {
  const fixture = makeFileRumi({ frameUnit: 3 });
  const wrong = await openRumi(fixture.file, { decoder: () => new Uint8Array(1) });
  await assert.rejects(() => wrong.read({ time: [0], bands: [0], window: [0, 0, 1, 1] }), /expected/);

  const limited = await openRumi(fixture.file, {
    decoder: identityDecoder,
    maxOutputBytes: 10,
  });
  await assert.rejects(() => limited.read({ time: [0], bands: [0], window: [0, 0, 3, 4] }), /maxOutputBytes/);
});
