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
  [/^(graffitiWallAtlas|billboardMural)/, [0.008, 0.015, 0.026], 0.60, 0, false],
  [/^(concrete|stoneWalk)Tex$/, [0.015, 0.018, 0.022], 0.55, 0.074],
  [/^grassTex_/, [0.026, 0.063, 0.008], 0.42, 0.045],
  [/^(metalPanel|carBody|truckCabPaint|truckCargoPaint)Tex$/, [0.012, 0.022, 0.036], 0.62, 0.063],
  [/^mp44MetalTex$/, [0.021, 0.026, 0.028], 0.79, 0.038],
  [/^(uspStipple|uspPolymer|knurl|vehicleRubber|vehicleDash)Tex$/, [0.016, 0.019, 0.022], 0.67, 0],
  [/^(woodCrate|mp44Wood)Tex$/, [0.039, 0.016, 0.007], 0.65, 0.041],
];

export function textureFinish(name) {
  const container = /^container(?:Door)?Tex_(green|blue|rust|red|gray)$/.exec(name);
  if (container) return { lift: steelLift[container[1]], sharpness: 0.64, wear: 0.093, repeat: true };
  const profile = profiles.find(([pattern]) => pattern.test(name));
  if (!profile) return null;
  const [, lift, sharpness, wear, repeat = true] = profile;
  return { lift, sharpness, wear, repeat };
}

function cell(x, y) {
  const pattern = Math.imul(x, 0x045d9f3b) ^ Math.imul(y, 0x119de1f3);
  const hash = Math.imul(pattern ^ (pattern >>> 16), 0x045d9f3b);
  return ((hash ^ (hash >>> 16)) & 255) / 255;
}

function weather(x, y, width, height) {
  // Seamless broad patches, stretched vertically.
  const u = 8 * x / width, v = 4 * y / height;
  const i = Math.floor(u), j = Math.floor(v);
  const s = u - i, t = v - j;
  const a = s * s * (3 - 2 * s), b = t * t * (3 - 2 * t);
  const nextI = (i + 1) % 8, nextJ = (j + 1) % 4;
  const upper = cell(i, j) * (1 - a) + cell(nextI, j) * a;
  const lower = cell(i, nextJ) * (1 - a) + cell(nextI, nextJ) * a;
  return upper * (1 - b) + lower * b - 0.5;
}

export function finishTexture(colors, width, height, { lift, sharpness, wear, repeat }) {
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
      const detail = clamp((tones[i] - blur) * sharpness, -9 / 255, 9 / 255);
      const exposure = clamp(0.5 + tones[i], 0.5, 1);
      const patina = wear ? weather(x, y, width, height) * wear : 0;
      for (let channel = 0; channel < 3; channel++) {
        const index = i * 3 + channel;
        colors[index] = clamp(colors[index] + lift[channel] * exposure + detail + patina, 0, 1);
      }
    }
  }
}
