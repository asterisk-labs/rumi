import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { epsgKind } from "../src/epsg.js";


test("keeps the JavaScript EPSG table in sync with the core", async () => {
  const path = new URL("../../../core/include/rumi/epsg_kinds.def", import.meta.url);
  const source = await readFile(path, "utf8");
  const expected = new Map();
  let kind;
  for (const line of source.split("\n")) {
    const begin = /RUMI_EPSG_BEGIN\(([^)]+)\)/.exec(line);
    if (begin) {
      kind = begin[1].toLowerCase();
      continue;
    }
    const range = /RUMI_EPSG_RANGE\([^,]+,\s*(\d+),\s*(\d+)\)/.exec(line);
    if (!range) continue;
    for (let code = Number(range[1]); code <= Number(range[2]); code += 1) {
      expected.set(code, kind);
    }
  }

  for (let code = 0; code <= 32766; code += 1) {
    assert.equal(epsgKind(code), expected.get(code), `EPSG:${code}`);
  }
});
