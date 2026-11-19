import { Color3 } from "@babylonjs/core/Maths/math.color";
import { DynamicTexture } from "@babylonjs/core/Materials/Textures/dynamicTexture";
import { PBRMaterial } from "@babylonjs/core/Materials/PBR/pbrMaterial";
import { StandardMaterial } from "@babylonjs/core/Materials/standardMaterial";
import { Texture } from "@babylonjs/core/Materials/Textures/texture";
import type { BaseTexture, Scene } from "@babylonjs/core";

// Procedural material kit. Every surface in the yard is painted at load time
// on a 2D canvas — zero external image assets, nothing to license — and
// turned into a full physically based material: the paint becomes the
// albedo, a height field (the paint's luminance, or a dedicated painter)
// becomes a tangent-space normal map, and the same height field drives an
// occlusion/roughness/metallic map so crevices sit darker and rougher while
// rain leaves a sheen on the high spots and standing water in the low ones.

export type Painter = (ctx: CanvasRenderingContext2D, size: number) => void;

export type RGB = readonly [number, number, number];

// A set of materials that differ only in paint (e.g. the container colours)
// can share one puddle field and one ORM map: the first material fills the
// holder, the rest reuse it. The holder is only trusted when the size, the
// roughness/metal/wet settings and the height field are exactly the same, so
// a mismatched caller falls back to its own maps instead of getting wrong ones.
export interface SharedWetMaps {
  sig?: string;
  height?: Float32Array;
  puddles?: Float32Array | null;
  orm?: DynamicTexture;
}

export interface CanvasMatOptions {
  rough: number; // base roughness 0..1 (1 = matte)
  metal?: number; // metallic 0..1
  u?: number;
  v?: number;
  // Relief strength for the generated normal map (0 = none). The height
  // field is the painted luminance unless `height` or `heightData` supplies one.
  bump?: number;
  bumpTex?: BaseTexture | null;
  height?: Painter;
  heightData?: Float32Array;
  roughVar?: number; // roughness swing from the height field (crevices rougher)
  wet?: number; // 0..1 rain: overall sheen plus standing water in the low areas
  emissive?: RGB;
  shared?: SharedWetMaps; // puddles + ORM reused across materials with the same relief
}

export interface FlatMatOptions {
  albedo: RGB;
  rough: number;
  metal?: number;
  emissive?: RGB;
  alpha?: number;
  tex?: BaseTexture | null;
  bump?: BaseTexture | null;
}

export interface StreakOptions {
  color: string;
  count: number;
  minW: number;
  maxW: number;
  minLen: number;
  maxLen: number;
  minAlpha: number;
  maxAlpha: number;
  minX?: number;
  maxX?: number;
  minY?: number;
  maxY?: number;
  fade?: boolean;
  rng?: () => number;
}

export function createPrng(seed: number): () => number {
  let state = seed >>> 0;
  return (): number => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
}

export function paintCanvas(size: number, paint: Painter): HTMLCanvasElement {
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  paint(canvas.getContext("2d", { willReadFrequently: true }) as CanvasRenderingContext2D, size);
  return canvas;
}

// Painted textures tile: DynamicTexture clamps by default, which smears the
// edge texels across anything scaled past 0..1
function tiling(tex: DynamicTexture): DynamicTexture {
  tex.wrapU = Texture.WRAP_ADDRESSMODE;
  tex.wrapV = Texture.WRAP_ADDRESSMODE;
  tex.anisotropicFilteringLevel = 8;
  return tex;
}

export function makeCanvasTexture(scene: Scene, name: string, size: number, paint: Painter, deferUpdate = false): DynamicTexture {
  const canvas = document.createElement("canvas");
  canvas.width = canvas.height = size;
  canvas.getContext("2d", { willReadFrequently: true });
  const tex = new DynamicTexture(name, canvas, scene, true);
  // Babylon may resize the backing canvas on WebGL 1 before painting.
  paint(tex.getContext() as CanvasRenderingContext2D, size);
  if (!deferUpdate) tex.update();
  return tiling(tex);
}

// Scene-cached texture: painted once, shared by every weapon that asks
export function cachedTexture(scene: Scene, name: string, make: () => DynamicTexture): DynamicTexture {
  const existing = scene.getTextureByName(name) as DynamicTexture | null;
  return existing ?? make();
}

// Scatter soft elliptical blotches — the workhorse for surface grime/variation
export function paintNoise(
  ctx: CanvasRenderingContext2D,
  size: number,
  colors: readonly string[],
  count: number,
  minR: number,
  maxR: number,
  alpha: number,
  rng: () => number = Math.random
): void {
  for (let i = 0; i < count; i++) {
    ctx.globalAlpha = alpha * (0.4 + rng() * 0.6);
    ctx.fillStyle = colors[(rng() * colors.length) | 0];
    const r = minR + rng() * (maxR - minR);
    ctx.beginPath();
    ctx.ellipse(rng() * size, rng() * size, r, r * (0.6 + rng() * 0.8), rng() * Math.PI, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.globalAlpha = 1;
}

// Vertical grime, rust or rain-runoff streaks
export function paintStreaks(ctx: CanvasRenderingContext2D, size: number, o: StreakOptions): void {
  const rng = o.rng ?? Math.random;
  const minX = o.minX ?? 0;
  const maxX = o.maxX ?? size;
  const minY = o.minY ?? 0;
  const maxY = o.maxY ?? size;
  for (let i = 0; i < o.count; i++) {
    const x = minX + rng() * (maxX - minX);
    const y = minY + rng() * (maxY - minY);
    const w = o.minW + rng() * (o.maxW - o.minW);
    const len = o.minLen + rng() * (o.maxLen - o.minLen);
    ctx.globalAlpha = o.minAlpha + rng() * (o.maxAlpha - o.minAlpha);
    if (o.fade) {
      const g = ctx.createLinearGradient(0, y, 0, y + len);
      g.addColorStop(0, o.color);
      g.addColorStop(1, "transparent");
      ctx.fillStyle = g;
    } else {
      ctx.fillStyle = o.color;
    }
    ctx.fillRect(x, y, w, len);
  }
  ctx.globalAlpha = 1;
}

// Short directional scratches across painted or metallic surfaces
export function paintScratches(
  ctx: CanvasRenderingContext2D,
  size: number,
  color: string,
  count: number,
  minLen: number,
  maxLen: number,
  alpha: number,
  lineWidth = 1,
  rng: () => number = Math.random
): void {
  ctx.globalAlpha = alpha;
  ctx.strokeStyle = color;
  ctx.lineWidth = lineWidth;
  for (let i = 0; i < count; i++) {
    const x = rng() * size;
    const y = rng() * size;
    const a = rng() * Math.PI * 2;
    const len = minLen + rng() * (maxLen - minLen);
    ctx.beginPath();
    ctx.moveTo(x, y);
    ctx.lineTo(x + Math.cos(a) * len, y + Math.sin(a) * len);
    ctx.stroke();
  }
  ctx.globalAlpha = 1;
}

// Soft low-frequency blotch field in [0,1] (white = high): the shape of
// standing water, worn patches, damp areas. Painted so it tiles.
export function paintBlotchField(ctx: CanvasRenderingContext2D, size: number, count: number, rMin: number, rMax: number): void {
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, size, size);
  for (let i = 0; i < count; i++) {
    const x = Math.random() * size;
    const y = Math.random() * size;
    const r = rMin + Math.random() * (rMax - rMin);
    for (const ox of [-size, 0, size]) {
      for (const oy of [-size, 0, size]) {
        const g = ctx.createRadialGradient(x + ox, y + oy, 0, x + ox, y + oy, r);
        g.addColorStop(0, "rgba(255,255,255,0.9)");
        g.addColorStop(0.55, "rgba(255,255,255,0.5)");
        g.addColorStop(1, "rgba(255,255,255,0)");
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.ellipse(x + ox, y + oy, r * (1 + Math.random() * 0.6), r * 0.7, Math.random() * Math.PI, 0, Math.PI * 2);
        ctx.fill();
      }
    }
  }
}

function luminance(canvas: HTMLCanvasElement): Float32Array {
  const s = canvas.width;
  const src = (canvas.getContext("2d") as CanvasRenderingContext2D).getImageData(0, 0, s, s).data;
  const h = new Float32Array(s * s);
  for (let i = 0; i < s * s; i++) {
    h[i] = (src[i * 4] * 0.299 + src[i * 4 + 1] * 0.587 + src[i * 4 + 2] * 0.114) / 255;
  }
  return h;
}

// Tangent-space normal map from a height field, Sobel filtered with
// wrap-around so tiled textures never seam. Written in the OpenGL/glTF
// convention (+X right, +Y up); Babylon's left-handed default wants X
// flipped, which the material flag handles.
export function normalMapFromHeight(scene: Scene, name: string, h: Float32Array, s: number, strength: number): DynamicTexture {
  const at = (x: number, y: number): number => h[((y + s) % s) * s + ((x + s) % s)];
  const tex = new DynamicTexture(name, { width: s, height: s }, scene, true);
  const ctx = tex.getContext() as CanvasRenderingContext2D;
  const out = ctx.createImageData(s, s);
  const d = out.data;
  for (let y = 0; y < s; y++) {
    for (let x = 0; x < s; x++) {
      const gx =
        at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) - (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
      const gy =
        at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) - (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
      let nx = -gx * strength;
      let ny = gy * strength; // canvas y grows downward; +Y up in tangent space
      let nz = 1;
      const len = Math.sqrt(nx * nx + ny * ny + nz * nz);
      nx /= len;
      ny /= len;
      nz /= len;
      const i = (y * s + x) * 4;
      d[i] = (nx * 0.5 + 0.5) * 255;
      d[i + 1] = (ny * 0.5 + 0.5) * 255;
      d[i + 2] = (nz * 0.5 + 0.5) * 255;
      d[i + 3] = 255;
    }
  }
  ctx.putImageData(out, 0, 0);
  tex.update();
  return tiling(tex);
}

// Occlusion / roughness / metallic packed the way PBRMaterial reads it
// (R = AO, G = roughness, B = metallic). Roughness = base, plus a swing from
// the height field (low = rougher), minus rain: a sheen everywhere and a
// near-mirror in the puddle blotches. AO darkens the crevices.
function ormFromHeight(
  scene: Scene,
  name: string,
  h: Float32Array,
  s: number,
  o: CanvasMatOptions,
  puddles: Float32Array | null
): DynamicTexture {
  const tex = new DynamicTexture(name, { width: s, height: s }, scene, true);
  const ctx = tex.getContext() as CanvasRenderingContext2D;
  const out = ctx.createImageData(s, s);
  const d = out.data;
  const roughVar = o.roughVar ?? 0.25;
  const wet = o.wet ?? 0;
  const metal = o.metal ?? 0;
  // crevice depth relative to a 2D-blurred neighbourhood reads as occlusion
  const r = 4;
  const span = r * 2 + 1;
  const tmp = new Float32Array(s * s);
  for (let y = 0; y < s; y++) {
    const row = y * s;
    let sum = 0;
    for (let k = -r; k <= r; k++) sum += h[row + (((k % s) + s) % s)];
    tmp[row] = sum / span;
    for (let x = 1; x < s; x++) {
      sum += h[row + ((x + r) % s)] - h[row + ((((x - r - 1) % s) + s) % s)];
      tmp[row + x] = sum / span;
    }
  }
  const blur = new Float32Array(s * s);
  for (let x = 0; x < s; x++) {
    let sum = 0;
    for (let k = -r; k <= r; k++) sum += tmp[(((k % s) + s) % s) * s + x];
    blur[x] = sum / span;
    for (let y = 1; y < s; y++) {
      sum += tmp[((y + r) % s) * s + x] - tmp[((((y - r - 1) % s) + s) % s) * s + x];
      blur[y * s + x] = sum / span;
    }
  }
  for (let i = 0; i < s * s; i++) {
    const height = h[i];
    const cavity = Math.max(0, blur[i] - height); // below the local mean
    const ao = 1 - Math.min(0.55, cavity * 2.2);
    let rough = o.rough + roughVar * (0.5 - height) - wet * 0.25;
    if (puddles) rough -= wet * puddles[i] * 0.75;
    rough = Math.max(0.03, Math.min(1, rough));
    d[i * 4] = ao * 255;
    d[i * 4 + 1] = rough * 255;
    d[i * 4 + 2] = metal * 255;
    d[i * 4 + 3] = 255;
  }
  ctx.putImageData(out, 0, 0);
  tex.update();
  return tiling(tex);
}

export interface StdMatOptions {
  tex?: BaseTexture | null;
  diffuse?: RGB;
  spec?: RGB;
  power?: number;
  emissive?: RGB;
}

// Legacy StandardMaterial helper (the weapon viewmodels' non-metal trim)
export function stdMat(scene: Scene, name: string, o: StdMatOptions): StandardMaterial {
  const mat = new StandardMaterial(name, scene);
  if (o.tex) mat.diffuseTexture = o.tex;
  if (o.diffuse) mat.diffuseColor = new Color3(o.diffuse[0], o.diffuse[1], o.diffuse[2]);
  if (o.spec) mat.specularColor = new Color3(o.spec[0], o.spec[1], o.spec[2]);
  if (o.power !== undefined) mat.specularPower = o.power;
  if (o.emissive) mat.emissiveColor = new Color3(o.emissive[0], o.emissive[1], o.emissive[2]);
  return mat;
}

// A flat-colour physically based material
export function flatMat(scene: Scene, name: string, o: FlatMatOptions): PBRMaterial {
  const cached = scene.getMaterialByName(name) as PBRMaterial | null;
  if (cached) return cached;
  const mat = new PBRMaterial(name, scene);
  mat.albedoColor = new Color3(o.albedo[0], o.albedo[1], o.albedo[2]);
  mat.roughness = o.rough;
  mat.metallic = o.metal ?? 0;
  if (o.tex) mat.albedoTexture = o.tex;
  if (o.bump) {
    mat.bumpTexture = o.bump;
    mat.invertNormalMapX = true;
    mat.invertNormalMapY = false;
  }
  if (o.emissive) mat.emissiveColor = new Color3(o.emissive[0], o.emissive[1], o.emissive[2]);
  if (o.alpha !== undefined) mat.alpha = o.alpha;
  mat.enableSpecularAntiAliasing = true;
  return mat;
}

// Cached canvas-painted PBR material: scene-cache lookup -> paint -> albedo
// + normal map + ORM map. The texture names derive from the material name.
export function canvasMat(scene: Scene, name: string, size: number, o: CanvasMatOptions, paint: Painter): PBRMaterial {
  const cached = scene.getMaterialByName(name);
  if (cached) return cached as PBRMaterial;
  const texName = name.replace("Mat", "Tex");
  const wet = o.wet ?? 0;
  const albedo = makeCanvasTexture(scene, texName, size, paint, wet > 0);
  const albedoCanvas = albedo.getContext().canvas as HTMLCanvasElement;
  const h = o.heightData ?? luminance(o.height ? paintCanvas(size, o.height) : albedoCanvas);
  const sig = `${size}|${o.rough}|${o.roughVar ?? 0.25}|${wet}|${o.metal ?? 0}`;
  const reuse = o.shared && o.shared.sig === sig && o.shared.height === h && o.shared.orm ? o.shared : null;

  // rain: standing water gathers in a blotch field; the albedo darkens
  // there too (wet ground is dark ground)
  let puddles: Float32Array | null = null;
  if (wet > 0) {
    puddles =
      reuse?.puddles ??
      luminance(paintCanvas(size, (ctx, s) => paintBlotchField(ctx, s, 5 + ((size / 128) | 0), size * 0.05, size * 0.16)));
    const ctx = albedo.getContext() as CanvasRenderingContext2D;
    const img = ctx.getImageData(0, 0, size, size);
    const d = img.data;
    for (let i = 0; i < size * size; i++) {
      const k = 1 - wet * (0.12 + 0.3 * puddles[i]);
      d[i * 4] *= k;
      d[i * 4 + 1] *= k;
      d[i * 4 + 2] *= k;
    }
    ctx.putImageData(img, 0, 0);
    albedo.update();
  }

  const mat = new PBRMaterial(name, scene);
  mat.albedoTexture = albedo;
  if (o.bumpTex) {
    mat.bumpTexture = o.bumpTex;
    mat.invertNormalMapX = true; // OpenGL-convention map in a left-handed scene
    mat.invertNormalMapY = false;
  } else if (o.bump) {
    mat.bumpTexture = normalMapFromHeight(scene, `${texName}_n`, h, size, o.bump);
    mat.invertNormalMapX = true; // OpenGL-convention map in a left-handed scene
    mat.invertNormalMapY = false;
  }
  const orm = reuse?.orm ?? ormFromHeight(scene, `${texName}_orm`, h, size, o, puddles);
  if (o.shared && !reuse) Object.assign(o.shared, { sig, height: h, puddles, orm });
  mat.metallicTexture = orm;
  mat.useAmbientOcclusionFromMetallicTextureRed = true;
  mat.useRoughnessFromMetallicTextureGreen = true;
  mat.useMetallnessFromMetallicTextureBlue = true;
  mat.useRoughnessFromMetallicTextureAlpha = false;
  mat.metallic = 1; // multipliers over the map
  mat.roughness = 1;
  if (o.emissive) mat.emissiveColor = new Color3(o.emissive[0], o.emissive[1], o.emissive[2]);
  mat.enableSpecularAntiAliasing = true;
  for (const tex of [albedo, o.bumpTex ? null : (mat.bumpTexture as DynamicTexture | null), orm]) {
    if (!tex) continue;
    if (o.u !== undefined) tex.uScale = o.u;
    if (o.v !== undefined) tex.vScale = o.v;
  }
  return mat;
}
