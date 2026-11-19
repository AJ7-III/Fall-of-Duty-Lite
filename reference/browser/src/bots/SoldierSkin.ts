import { Constants } from "@babylonjs/core/Engines/constants";
import { DynamicTexture } from "@babylonjs/core/Materials/Textures/dynamicTexture";
import { Texture } from "@babylonjs/core/Materials/Textures/texture";
import type { BaseTexture, Scene } from "@babylonjs/core";
import type { SoldierTint } from "./SoldierBody";

// The soldier's paint job, rebuilt at runtime from the glTF's own albedo.
// The Vanguard ships as sci-fi plate in beige with red trim; instead of
// painting camo blotches over that (which buried every strap, buckle,
// scratch and plate edge the artist drew), this keeps all of that detail
// and recolours it per faction: OPFOR in olive drab with faded red unit
// stripes, the player in slate khaki with steel-blue stripes. A soft
// large-scale camo mottle and fine fabric grain go on top of the plates
// only, and a roughness/metal map is derived so plates read matte, straps
// rougher and buckles metallic.

interface Recolor {
  plateHue: number; // degrees
  plateSat: number; // multiplier
  plateLight: number; // multiplier
  stripeHue: number;
  stripeSat: number;
  stripeLight: number;
  camoA: string; // mottle tones (rgba multiplied over the plates)
  camoB: string;
}

const RECOLOR: Record<SoldierTint, Recolor> = {
  opfor: {
    plateHue: 82,
    plateSat: 0.5,
    plateLight: 0.6,
    stripeHue: 8,
    stripeSat: 0.75,
    stripeLight: 0.75,
    camoA: "rgba(40,60,30,0.35)",
    camoB: "rgba(120,130,80,0.3)",
  },
  player: {
    // dark slate plate — the "operator" kit, unmistakable next to OPFOR olive
    plateHue: 205,
    plateSat: 0.12,
    plateLight: 0.46,
    stripeHue: 208,
    stripeSat: 0.55,
    stripeLight: 0.85,
    camoA: "rgba(20,24,30,0.4)",
    camoB: "rgba(120,125,120,0.25)",
  },
};

const HSL_TMP = new Float64Array(3);
const RGB_TMP = new Float64Array(3);

function rgbToHsl(r: number, g: number, b: number, out: Float64Array): void {
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const l = (max + min) / 2;
  if (max === min) {
    out[0] = 0;
    out[1] = 0;
    out[2] = l;
    return;
  }
  const d = max - min;
  const s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
  let h: number;
  if (max === r) h = (g - b) / d + (g < b ? 6 : 0);
  else if (max === g) h = (b - r) / d + 2;
  else h = (r - g) / d + 4;
  out[0] = h * 60;
  out[1] = s;
  out[2] = l;
}

function hslToRgb(h: number, s: number, l: number, out: Float64Array): void {
  const c = (1 - Math.abs(2 * l - 1)) * s;
  const hp = (((h % 360) + 360) % 360) / 60;
  const x = c * (1 - Math.abs((hp % 2) - 1));
  let r = 0;
  let g = 0;
  let b = 0;
  if (hp < 1) {
    r = c;
    g = x;
  } else if (hp < 2) {
    r = x;
    g = c;
  } else if (hp < 3) {
    g = c;
    b = x;
  } else if (hp < 4) {
    g = x;
    b = c;
  } else if (hp < 5) {
    r = x;
    b = c;
  } else {
    r = c;
    b = x;
  }
  const m = l - c / 2;
  out[0] = r + m;
  out[1] = g + m;
  out[2] = b + m;
}

// Camo mottle + fabric grain, painted once and sampled per texel
function detailLayer(size: number, recolor: Recolor): Uint8ClampedArray {
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = size;
  const ctx = canvas.getContext("2d", { willReadFrequently: true }) as CanvasRenderingContext2D;
  ctx.fillStyle = "#808080";
  ctx.fillRect(0, 0, size, size);
  for (let i = 0; i < 90; i++) {
    ctx.fillStyle = i % 2 ? recolor.camoA : recolor.camoB;
    ctx.beginPath();
    ctx.ellipse(
      Math.random() * size,
      Math.random() * size,
      size * (0.02 + Math.random() * 0.06),
      size * (0.015 + Math.random() * 0.04),
      Math.random() * Math.PI,
      0,
      Math.PI * 2
    );
    ctx.fill();
  }
  // fabric / plate grain
  const img = ctx.getImageData(0, 0, size, size);
  const d = img.data;
  for (let i = 0; i < d.length; i += 4) {
    const n = (Math.random() - 0.5) * 18;
    d[i] += n;
    d[i + 1] += n;
    d[i + 2] += n;
  }
  return d;
}

export interface TexturePixels {
  data: Uint8Array;
  w: number;
  h: number;
}

const pixelCache = new WeakMap<BaseTexture, Promise<TexturePixels | null>>();

export function readTexturePixels(source: BaseTexture): Promise<TexturePixels | null> {
  const existing = pixelCache.get(source);
  if (existing) return existing;
  const size = source.getSize();
  const w = size.width;
  const h = size.height;
  if (!w || !h) return Promise.resolve(null);
  const raw = source.readPixels();
  if (!raw) return Promise.resolve(null);
  const job = raw
    .then((buf) => {
      setTimeout(() => pixelCache.delete(source), 0);
      return buf ? { data: buf as Uint8Array, w, h } : null;
    })
    .catch((err) => {
      pixelCache.delete(source);
      throw err;
    });
  pixelCache.set(source, job);
  return job;
}

export interface SoldierSkinTextures {
  albedo: DynamicTexture;
  orm: DynamicTexture;
}

const cache = new WeakMap<Scene, Map<SoldierTint, Promise<SoldierSkinTextures>>>();

// Reads the glTF albedo back from the GPU (the loader already uploaded it),
// recolours it and packs a matching roughness/metal map. Cached per scene
// and faction; bodies swap the textures in when the promise resolves.
export function soldierSkinTextures(scene: Scene, tint: SoldierTint, source: BaseTexture): Promise<SoldierSkinTextures> {
  let textures = cache.get(scene);
  if (!textures) {
    textures = new Map();
    cache.set(scene, textures);
  }
  const existing = textures.get(tint);
  if (existing) return existing;
  const job = buildTextures(scene, tint, source).catch((error) => {
    textures.delete(tint);
    throw error;
  });
  textures.set(tint, job);
  return job;
}

async function buildTextures(scene: Scene, tint: SoldierTint, source: BaseTexture): Promise<SoldierSkinTextures> {
  const pixels = await readTexturePixels(source);
  if (!pixels) throw new Error("Soldier skin source texture has no pixels");
  if (scene.isDisposed) throw new Error("Soldier skin scene was disposed during readback");
  const { data: src, w, h } = pixels;
  const recolor = RECOLOR[tint];
  const detail = detailLayer(w, recolor);

  const albedo = new DynamicTexture(
    `soldierAlbedo_${tint}`,
    { width: w, height: h },
    scene,
    true,
    Texture.TRILINEAR_SAMPLINGMODE,
    Constants.TEXTUREFORMAT_RGBA,
    false // readPixels rows are bottom-up; upload unflipped to match the source
  );
  const orm = new DynamicTexture(
    `soldierOrm_${tint}`,
    { width: w, height: h },
    scene,
    true,
    Texture.TRILINEAR_SAMPLINGMODE,
    Constants.TEXTUREFORMAT_RGBA,
    false
  );
  const actx = albedo.getContext() as CanvasRenderingContext2D;
  const octx = orm.getContext() as CanvasRenderingContext2D;
  const aimg = actx.createImageData(w, h);
  const oimg = octx.createImageData(w, h);
  const a = aimg.data;
  const o = oimg.data;

  for (let i = 0; i < w * h; i++) {
    const p = i * 4;
    const r = src[p] / 255;
    const g = src[p + 1] / 255;
    const b = src[p + 2] / 255;
    rgbToHsl(r, g, b, HSL_TMP);
    const hue = HSL_TMP[0];
    const sat = HSL_TMP[1];
    const light = HSL_TMP[2];
    const isRed = sat > 0.45 && (hue < 20 || hue > 340) && light > 0.2;
    const isPlate = !isRed && sat > 0.18 && hue > 15 && hue < 55 && light > 0.3;
    const isMetal = !isRed && sat < 0.12 && light > 0.25 && light < 0.7;

    let nr = r;
    let ng = g;
    let nb = b;
    let rough = 0.75;
    let metal = 0;
    if (isPlate) {
      // the row/col of this texel in the detail layer is the same texel:
      // the detail canvas is drawn top-down while the readback is bottom-up,
      // which only mirrors the mottle — it is random anyway
      const dv = detail[p] / 128; // 1 = neutral
      hslToRgb(recolor.plateHue, Math.min(1, sat * recolor.plateSat), Math.min(0.92, light * recolor.plateLight * dv), RGB_TMP);
      nr = RGB_TMP[0];
      ng = RGB_TMP[1];
      nb = RGB_TMP[2];
      rough = 0.5 + (1 - light) * 0.3; // scuffed lows rougher than the polished highs
      metal = 0.08;
    } else if (isRed) {
      hslToRgb(recolor.stripeHue, Math.min(1, sat * recolor.stripeSat), Math.min(0.9, light * recolor.stripeLight), RGB_TMP);
      nr = RGB_TMP[0];
      ng = RGB_TMP[1];
      nb = RGB_TMP[2];
      rough = 0.55;
    } else if (isMetal) {
      const k = 0.9;
      nr = r * k;
      ng = g * k;
      nb = b * k;
      rough = 0.35;
      metal = 0.85;
    } else {
      // straps, webbing, dark fabric: keep, a touch darker and cooler
      nr = r * 0.85;
      ng = g * 0.87;
      nb = b * 0.9;
      rough = 0.82;
    }
    a[p] = nr * 255;
    a[p + 1] = ng * 255;
    a[p + 2] = nb * 255;
    a[p + 3] = 255;
    o[p] = 255;
    o[p + 1] = rough * 255;
    o[p + 2] = metal * 255;
    o[p + 3] = 255;
  }
  actx.putImageData(aimg, 0, 0);
  octx.putImageData(oimg, 0, 0);
  albedo.update(false);
  orm.update(false);
  albedo.anisotropicFilteringLevel = 8;
  orm.anisotropicFilteringLevel = 8;
  return { albedo, orm };
}
