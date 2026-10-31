import assert from "node:assert/strict";
import test from "node:test";

import { affineCorners, rasterRgba } from "../playground/render.js";

test("maps image corners through a rotated affine transform", () => {
  assert.deepEqual(
    affineCorners([2, 0.5, 10, -0.25, -3, 20], 4, 2),
    [[10, 20], [18, 19], [19, 13], [11, 14]],
  );
});

test("stretches one band to grayscale", () => {
  const image = rasterRgba(new Uint16Array([0, 10, 20, 30]), 2, 2, 1);
  assert.deepEqual([image.width, image.height], [2, 2]);
  assert.deepEqual([...image.rgba], [
    0, 0, 0, 255,
    85, 85, 85, 255,
    170, 170, 170, 255,
    255, 255, 255, 255,
  ]);
});

test("renders planar RGB and limits the preview size", () => {
  const data = new Uint8Array([
    0, 255, 0, 255,
    0, 0, 255, 255,
    0, 0, 0, 255,
  ]);
  const image = rasterRgba(data, 2, 2, 3, 1);
  assert.deepEqual([image.width, image.height], [1, 1]);
  assert.deepEqual([...image.rgba], [0, 0, 0, 255]);
});
