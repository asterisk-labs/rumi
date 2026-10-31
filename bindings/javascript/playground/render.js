const MAX_SAMPLES = 50_000;

function numeric(value) {
  return typeof value === "bigint" ? Number(value) : value;
}

function limits(data, offset, pixels) {
  const step = Math.max(1, Math.ceil(pixels / MAX_SAMPLES));
  const sample = [];
  for (let index = 0; index < pixels; index += step) {
    const value = numeric(data[offset + index]);
    if (Number.isFinite(value)) sample.push(value);
  }
  if (sample.length === 0) return [0, 1];
  sample.sort((a, b) => a - b);
  const low = sample[Math.floor((sample.length - 1) * 0.02)];
  const high = sample[Math.ceil((sample.length - 1) * 0.98)];
  return low === high ? [low - 0.5, high + 0.5] : [low, high];
}

function byte(value, [low, high]) {
  if (!Number.isFinite(value)) return 0;
  return Math.round(Math.max(0, Math.min(1, (value - low) / (high - low))) * 255);
}

export function rasterRgba(data, width, height, planes, maxSize = 2048) {
  const pixels = width * height;
  if (planes !== 1 && planes !== 3) throw new Error("rendering needs one or three bands");
  const scale = Math.min(1, maxSize / Math.max(width, height));
  const outWidth = Math.max(1, Math.round(width * scale));
  const outHeight = Math.max(1, Math.round(height * scale));
  const stretches = Array.from({ length: planes }, (_, plane) => limits(data, plane * pixels, pixels));
  const rgba = new Uint8ClampedArray(outWidth * outHeight * 4);

  for (let y = 0; y < outHeight; y += 1) {
    const sourceY = Math.min(height - 1, Math.floor(y / scale));
    for (let x = 0; x < outWidth; x += 1) {
      const sourceX = Math.min(width - 1, Math.floor(x / scale));
      const sourcePixel = sourceY * width + sourceX;
      const target = (y * outWidth + x) * 4;
      if (planes === 1) {
        const value = byte(numeric(data[sourcePixel]), stretches[0]);
        rgba[target] = value;
        rgba[target + 1] = value;
        rgba[target + 2] = value;
      } else {
        for (let channel = 0; channel < 3; channel += 1) {
          rgba[target + channel] = byte(
            numeric(data[channel * pixels + sourcePixel]),
            stretches[channel],
          );
        }
      }
      rgba[target + 3] = 255;
    }
  }
  return { rgba, width: outWidth, height: outHeight, stretches };
}

export function affineCorners(transform, width, height) {
  const [a, b, c, d, e, f] = transform;
  return [[0, 0], [width, 0], [width, height], [0, height]].map(
    ([column, row]) => [a * column + b * row + c, d * column + e * row + f],
  );
}
