const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const luma = (r, g, b) => 0.2126 * r + 0.7152 * g + 0.0722 * b;

const steel = {
  green: [-0.022, 0.066, 0.071],
  blue: [0, 0.037, 0.061],
  rust: [0.080, 0.023, -0.002],
  red: [0.060, 0.016, 0.001],
  gray: [0.008, 0.025, 0.043],
};

export function textureLift(name) {
  const container = name.match(/^container(?:Door)?Tex_(green|blue|rust|red|gray)$/);
  if (container) return steel[container[1]];
  if (name === "graffitiWallAtlasTex" || name === "concreteTex") return [0.005, 0.018, 0.032];
  if (name === "stoneWalkTex") return [0.016, 0.017, 0.033];
  if (name.startsWith("grassTex_")) return [0.030, 0.069, 0.005];
  if (/^(metalPanel|carBody|truckCabPaint|truckCargoPaint)Tex$/.test(name)) return [0.002, 0.018, 0.041];
  if (name === "mp44MetalTex") return [0.004, 0.010, 0.020];
  if (/^(woodCrate|mp44Wood)Tex$/.test(name)) return [0.027, 0.012, 0.003];
  if (name.startsWith("billboardMuralTex_")) return [0.009, 0.012, 0.020];
  return null;
}

// Work at the actual exported size. Keep the source detail and limit the
// unsharp mask so letter strokes and seams avoid bright halos in RGB565.
export function finishTexture(colors, width, height, lift, repeat) {
  const tones = new Float32Array(width * height);
  for (let i = 0; i < tones.length; i++) tones[i] = luma(colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]);

  for (let y = 0; y < height; y++) {
    const up = y > 0 ? y - 1 : repeat ? height - 1 : y;
    const down = y + 1 < height ? y + 1 : repeat ? 0 : y;
    for (let x = 0; x < width; x++) {
      const left = x > 0 ? x - 1 : repeat ? width - 1 : x;
      const right = x + 1 < width ? x + 1 : repeat ? 0 : x;
      const i = y * width + x;
      const blur = (tones[y * width + left] + tones[y * width + right] + tones[up * width + x] + tones[down * width + x]) / 4;
      const detail = clamp((tones[i] - blur) * 0.5, -8 / 255, 8 / 255);
      const exposure = clamp(0.5 + tones[i], 0.5, 1);
      for (let channel = 0; channel < 3; channel++) {
        const index = i * 3 + channel;
        colors[index] = clamp(colors[index] + lift[channel] * exposure + detail, 0, 1);
      }
    }
  }
}
