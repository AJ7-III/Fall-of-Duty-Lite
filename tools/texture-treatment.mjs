const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const luma = (r, g, b) => 0.2126 * r + 0.7152 * g + 0.0722 * b;

const steelLift = {
  green: [-0.015, 0.062, 0.060],
  blue: [0.003, 0.041, 0.063],
  rust: [0.083, 0.028, 0.006],
  red: [0.060, 0.023, 0.006],
  gray: [0.020, 0.031, 0.046],
};

// The profiles keep printed surfaces intact and reserve wear for materials
// that should show oxidation, dirt or abrasion.
const profiles = [
  [/^(graffitiWallAtlas|billboardMural)/, [0.008, 0.015, 0.026], 0.60, 0, [0, 0, 0], false],
  [/^(concrete|stoneWalk)Tex$/, [0.014, 0.025, 0.036], 0.61, 0.098, [0.42, 0.03, -0.43]],
  [/^grassTex_/, [0.030, 0.058, 0.007], 0.45, 0.077, [0.64, 0.16, -0.31]],
  [/^(metalPanel|carBody|truckCabPaint|truckCargoPaint)Tex$/, [0.012, 0.025, 0.042], 0.64, 0.074, [0.51, 0.11, -0.52]],
  [/^mp44MetalTex$/, [0.020, 0.028, 0.043], 0.73, 0.050, [0.24, 0, -0.35]],
  [/^(uspStipple|uspPolymer|knurl|vehicleRubber|vehicleDash)Tex$/, [0.016, 0.019, 0.025], 0.67, 0, [0, 0, 0]],
  [/^(woodCrate|mp44Wood)Tex$/, [0.042, 0.018, 0.006], 0.66, 0.078, [0.45, 0.11, -0.20]],
];

export function textureFinish(name) {
  const container = /^container(?:Door)?Tex_(green|blue|rust|red|gray)$/.exec(name);
  if (container) {
    return { lift: steelLift[container[1]], sharpness: .64, wear: .12, tint: [.68, .14, -.55],
             saturation: container[1] === "gray" ? 1 : 1.20, repeat: true };
  }
  const profile = profiles.find(([pattern]) => pattern.test(name));
  if (!profile) return null;
  const [, lift, sharpness, wear, tint, repeat = true] = profile;
  return { lift, sharpness, wear, tint, repeat };
}

function cell(x, y) {
  const pattern = Math.imul(x, 0x045d9f3b) ^ Math.imul(y, 0x119de1f3);
  const hash = Math.imul(pattern ^ (pattern >>> 16), 0x045d9f3b);
  return ((hash ^ (hash >>> 16)) & 255) / 255;
}

function weather(x, y, width, height, columns, rows, seed) {
  const u = columns * x / width, v = rows * y / height;
  const i = Math.floor(u), j = Math.floor(v);
  const s = u - i, t = v - j;
  const a = s * s * (3 - 2 * s), b = t * t * (3 - 2 * t);
  const nextI = (i + 1) % columns, nextJ = (j + 1) % rows;
  const upper = cell(i + seed, j) * (1 - a) + cell(nextI + seed, j) * a;
  const lower = cell(i + seed, nextJ) * (1 - a) + cell(nextI + seed, nextJ) * a;
  return upper * (1 - b) + lower * b - 0.5;
}

export function finishTexture(colors, width, height, { lift, sharpness, wear, tint, saturation = 1, repeat }) {
  const tones = new Float32Array(width * height);
  for (let i = 0; i < tones.length; i++) tones[i] = luma(colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]);

  for (let y = 0; y < height; y++) {
    const up = y > 0 ? y - 1 : repeat ? height - 1 : y;
    const down = y + 1 < height ? y + 1 : repeat ? 0 : y;
    for (let x = 0; x < width; x++) {
      const left = x > 0 ? x - 1 : repeat ? width - 1 : x;
      const right = x + 1 < width ? x + 1 : repeat ? 0 : x;
      const i = y * width + x;
      const west = tones[y * width + left], east = tones[y * width + right];
      const north = tones[up * width + x], south = tones[down * width + x];
      const blur = (west + east + north + south) / 4;
      const detail = clamp((tones[i] - blur) * sharpness, -9 / 255, 9 / 255);
      const exposure = clamp(0.5 + tones[i], 0.5, 1);
      // Let scuffs catch light from one side. Two scales keep the wet patches
      // visible after mip filtering without adding grain to painted signs.
      const patina = wear ? wear * (.78 * weather(x, y, width, height, 8, 4, 2) +
                                   .42 * weather(x, y, width, height, 23, 19, 17)) : 0;
      const bevel = wear ? clamp((west - east) * .14 + (north - south) * .23, -.027, .027) : 0;
      for (let channel = 0; channel < 3; channel++) {
        const index = i * 3 + channel;
        const pigment = tones[i] + (colors[index] - tones[i]) * saturation;
        colors[index] = clamp(pigment + lift[channel] * exposure + detail + bevel + patina * (1 + tint[channel]), 0, 1);
      }
    }
  }
}
