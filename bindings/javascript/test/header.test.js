import assert from "node:assert/strict";
import test from "node:test";

import { parseHeader } from "../src/header.js";
import { baseOffset, makeHeader, makeRumi } from "./fixture.js";

test("parses canonical variable counts and reconstructs offsets", () => {
  const fixture = makeRumi({ frameUnit: 3 });
  const header = parseHeader(fixture.header);
  assert.equal(header.frameCount, fixture.frames.length);
  assert.equal(header.baseFrameOffset, BigInt(baseOffset(3, fixture.frames.length)));
  let offset = header.baseFrameOffset;
  for (let i = 0; i < fixture.frames.length; i += 1) {
    assert.equal(header.frameOffset(i), offset);
    assert.equal(header.frameByteCount(i), fixture.frames[i].byteLength);
    offset += BigInt(fixture.frames[i].byteLength);
  }
});

test("keeps constant counts implicit", () => {
  const bytes = makeHeader({
    width: 2,
    height: 2,
    times: 1,
    tileWidth: 2,
    tileHeight: 2,
    bands: 1,
    frameUnit: 0,
    counts: [8],
  });
  const header = parseHeader(bytes);
  assert.equal(header.countBits, 0);
  assert.equal(header.frameByteCount(0), 8);
});

test("rejects malformed fixed fields", () => {
  const valid = makeHeader({
    width: 2,
    height: 2,
    times: 1,
    tileWidth: 2,
    tileHeight: 2,
    bands: 1,
    frameUnit: 0,
    counts: [8],
  });
  for (const [offset, value, message] of [
    [0, 0, /magic/],
    [4, 2, /version/],
    [6, 0, /dimensions/],
    [26, 10, /frameUnit/],
    [31, 33, /countBits/],
  ]) {
    const broken = valid.slice();
    if (offset === 0 || offset === 6) new DataView(broken.buffer).setUint32(offset, value, true);
    else if (offset === 4) new DataView(broken.buffer).setUint16(offset, value, true);
    else broken[offset] = value;
    assert.throws(() => parseHeader(broken), message);
  }
});

test("rejects non-canonical count packing", () => {
  const bytes = makeHeader({
    width: 4,
    height: 1,
    times: 1,
    tileWidth: 1,
    tileHeight: 1,
    bands: 1,
    frameUnit: 0,
    counts: [4, 5, 4, 5],
  });
  const tooWide = bytes.slice();
  tooWide[31] = 2;
  tooWide[32] = 0b01000100;
  assert.throws(() => parseHeader(tooWide), /canonically/);

  const padded = bytes.slice();
  padded[32] |= 0x80;
  assert.throws(() => parseHeader(padded), /padding/);
});

test("enforces the decoded frame limit before decoding", () => {
  const fixture = makeRumi({ width: 4, height: 3, tileWidth: 4, tileHeight: 3 });
  assert.throws(() => parseHeader(fixture.header, { maxFrameBytes: 143 }), /maxFrameBytes/);
  assert.equal(parseHeader(fixture.header, { maxFrameBytes: 144 }).maxDecodedFrame, 144);
});
