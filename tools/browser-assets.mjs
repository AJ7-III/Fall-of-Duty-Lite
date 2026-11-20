// Runs inside a private Vite served page. Only this exporter touches the
// browser builders' private fields. Shipped browser code has no exporter hooks.
import { Engine } from "@babylonjs/core/Engines/engine";
import { Scene } from "@babylonjs/core/scene";
import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import { Input } from "../reference/browser/src/engine/Input.ts";
import { WorldMaterials } from "../reference/browser/src/world/materials/WorldMaterials.ts";
import { ShipBoxMap } from "../reference/browser/src/world/ShipBoxMap.ts";
import { PlayerController } from "../reference/browser/src/player/PlayerController.ts";
import { CameraRig } from "../reference/browser/src/player/CameraRig.ts";
import { BotNav } from "../reference/browser/src/bots/BotNav.ts";
import { ViewModelRig } from "../reference/browser/src/rendering/ViewModelRig.ts";
import { BoltActionSniper } from "../reference/browser/src/weapons/BoltActionSniper.ts";
import { WeaponManager } from "../reference/browser/src/weapons/WeaponManager.ts";
import { Apache } from "../reference/browser/src/killstreaks/Apache.ts";
import { Killstreaks } from "../reference/browser/src/killstreaks/Killstreaks.ts";
import { AudioSynth } from "../reference/browser/src/rendering/AudioSynth.ts";
import { buildFieldInfantry, buildViewArms, registerInfantryFaces, registerInfantryMaterials } from "./lite-infantry.mjs";
import { buildLiteSniper, sampleBoltClearance } from "./lite-sniper-stock.mjs";
import { finishTexture, textureLift } from "./texture-treatment.mjs";
import duelHitboxes from "./rival-hitboxes.json";

const audioHz = 24000;
const chunkSize = 4;
const to565 = (r, g, b) =>
  (Math.min(31, Math.round(r * 31)) << 11) | (Math.min(63, Math.round(g * 63)) << 5) | Math.min(31, Math.round(b * 31));

function seededRandom(seed) {
  return () => {
    seed ^= seed << 13;
    seed ^= seed >>> 17;
    seed ^= seed << 5;
    return (seed >>> 0) / 4294967296;
  };
}

function base64(typed) {
  const bytes = new Uint8Array(typed.buffer, typed.byteOffset, typed.byteLength);
  let result = "";
  // Split before conversion to stay below browser argument limits.
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    result += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return btoa(result);
}

function liteTextureTone(name) {
  if (name === "containerTex_green" || name === "containerDoorTex_green") return [0.43, 1.06, 1.00, 0.93];
  return null;
}

const weaponAlbedo = {
  mp44MetalMat: [2.28, 2.32, 2.24],
  mp44DarkMat: [.18, .20, .20],
  mp44WornEdgeMat: [.37, .41, .41],
  mp44WoodMat: [1.49, 1.33, 1.16],
  sniperMetalMat: [.25, .29, .30],
  sniperScopeMat: [.16, .19, .19],
  uspSlideMat: [.24, .28, .29],
  uspSteelMat: [.39, .43, .45],
};

// Bake the browser's material color into each tile. We store sample rows in
// texture V order (as returned by WebGL), compatible with the software sampler.
class Palette {
  materials = [];
  textures = [];
  idByMaterial = new Map();

  constructor(maxTexture) {
    this.maxTexture = maxTexture;
  }

  async material(mat) {
    if (this.idByMaterial.has(mat)) return this.idByMaterial.get(mat);
    const id = this.materials.length;
    this.idByMaterial.set(mat, id);
    const finish = weaponAlbedo[mat?.name];
    const albedo = finish ? { r: finish[0], g: finish[1], b: finish[2] }
      : mat?.albedoColor ?? mat?.diffuseColor ?? { r: 0.45, g: 0.45, b: 0.45 };
    const emissive = mat?.emissiveColor ?? { r: 0, g: 0, b: 0 };
    const color = mat?.name === "lampHeadMat"
      ? to565(1.0, 0.76, 0.42)
      : to565(albedo.r * 0.86 + emissive.r, albedo.g * 0.89 + emissive.g, albedo.b * 0.92 + emissive.b);
    const variant = {
      name: mat?.name ?? "untextured",
      color,
      texture: -1,
      alpha: Math.round(255 * (mat?.alpha ?? 1)),
      doubleSided: true,
    };
    this.materials.push(variant);

    const tex = mat?.albedoTexture ?? mat?.diffuseTexture;
    if (!tex || !tex.isReady()) return id;
    const raw = await tex.readPixels();
    if (!raw) return id;
    const { width, height } = tex.getSize();
    // Four separate wall strips share this wide atlas. Each needs enough
    // source texels for readable near-wall artwork.
    const limit = variant.name.includes("graffiti")
      ? Math.min(1024, this.maxTexture * 8)
      : variant.name.includes("billboard")
        ? this.maxTexture * 2
        : this.maxTexture;
    const shrink = Math.max(1, Math.max(width, height) / limit);
    const targetW = Math.max(1, Math.round(width / shrink));
    const targetH = Math.max(1, Math.round(height / shrink));
    const pixels = new Uint16Array(targetW * targetH);
    const range = raw instanceof Float32Array ? 1 : 255;
    const tone = liteTextureTone(tex.name);
    const lift = textureLift(tex.name);
    const colors = lift ? new Float32Array(pixels.length * 3) : null;
    for (let y = 0; y < targetH; y++) {
      const fromY = Math.floor((y * height) / targetH);
      const toY = Math.floor(((y + 1) * height) / targetH);
      for (let x = 0; x < targetW; x++) {
        const fromX = Math.floor((x * width) / targetW);
        const toX = Math.floor(((x + 1) * width) / targetW);
        let r = 0,
          g = 0,
          b = 0;
        for (let sy = fromY; sy < toY; sy++)
          for (let sx = fromX; sx < toX; sx++) {
            const i = (sy * width + sx) * 4;
            r += raw[i];
            g += raw[i + 1];
            b += raw[i + 2];
          }
        const divisor = range * (toX - fromX) * (toY - fromY);
        r = (r / divisor) * albedo.r * 0.86 + emissive.r;
        g = (g / divisor) * albedo.g * 0.89 + emissive.g;
        b = (b / divisor) * albedo.b * 0.92 + emissive.b;
        if (tone) {
          const [source, red, green, blue] = tone;
          const gray = (0.299 * r + 0.587 * g + 0.114 * b) * (1 - source);
          r = r * source + gray * red;
          g = g * source + gray * green;
          b = b * source + gray * blue;
        }
        const i = y * targetW + x;
        if (colors) {
          colors[i * 3] = r;
          colors[i * 3 + 1] = g;
          colors[i * 3 + 2] = b;
        } else {
          pixels[i] = to565(r, g, b);
        }
      }
    }
    if (colors) {
      finishTexture(colors, targetW, targetH, lift, !/^(graffitiWallAtlas|billboardMural)/.test(tex.name));
      for (let i = 0; i < pixels.length; i++) pixels[i] = to565(colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]);
    }
    variant.texture = this.textures.length;
    this.textures.push({ name: tex.name, width: targetW, height: targetH, pixels: base64(pixels) });
    return id;
  }
}

function toChunk(vertices, triangles, first, count, stride = 3) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let i = first; i < first + count; i++) {
    for (let n = 0; n < 3; n++) {
      const base = triangles[i * stride + n] * 5;
      for (let d = 0; d < 3; d++) {
        min[d] = Math.min(min[d], vertices[base + d]);
        max[d] = Math.max(max[d], vertices[base + d]);
      }
    }
  }
  return [...min, ...max, first, count];
}

function partition(mesh, sizes, spatial) {
  const buckets = new Map();
  for (let i = 0; i < mesh.triangles.length; i += sizes) {
    const indices = mesh.triangles.slice(i, i + sizes);
    let bucketKey = "";
    if (spatial) {
      const samples = indices.slice(0, 3).map((index) => index * 5);
      const midpoint = (axis) => samples.reduce((sum, index) => sum + mesh.vertices[index + axis], 0) / 3;
      bucketKey = `${Math.floor(midpoint(0) / chunkSize)},${Math.floor(midpoint(2) / chunkSize)}`;
    }
    let bucket = buckets.get(bucketKey);
    if (!bucket) buckets.set(bucketKey, (bucket = []));
    bucket.push(...indices);
  }
  const triangles = [];
  const chunks = [];
  for (const bucket of buckets.values()) {
    const count = bucket.length / sizes;
    chunks.push(
      toChunk(mesh.vertices, bucket, 0, count, sizes)
        .slice(0, 6)
        .concat(triangles.length / sizes, count)
    );
    for (const value of bucket) triangles.push(value);
  }
  return { vertices: mesh.vertices, triangles, chunks };
}

function addGrassTufts(mesh, palette, instances) {
  const yard = instances.filter(({ m }) => Math.max(Math.abs(m[12]), Math.abs(m[14])) < 16);
  if (!yard.length) return;

  const material = palette.materials.length;
  for (const [r, g, b] of [[0.20, 0.31, 0.16], [0.31, 0.43, 0.21], [0.42, 0.50, 0.27]]) {
    palette.materials.push({ color: to565(r, g, b), texture: -1, alpha: 255, doubleSided: true });
  }

  const random = seededRandom(0x72162c4f);
  // Sample one in three of the browser's valid yard placements. Three tapered
  // blades give each clump a silhouette using the existing opaque mesh format.
  for (let i = 0; i < yard.length; i += 3) {
    const { m } = yard[i + Math.floor(random() * Math.min(3, yard.length - i))];
    const rotation = random() * 2 * Math.PI;
    for (let blade = 0; blade < 3; blade++) {
      const angle = rotation + blade * (2 * Math.PI / 3);
      const dx = Math.cos(angle), dz = Math.sin(angle);
      const rootX = m[12] + dx * 0.055, rootZ = m[14] + dz * 0.055;
      const width = 0.044 + 0.023 * random();
      const height = (0.21 + 0.15 * random()) * m[5];
      const lean = height * (0.28 + 0.30 * random());
      const first = mesh.vertices.length / 5;
      mesh.vertices.push(
        rootX + dz * width, 0, rootZ - dx * width, 0, 0,
        rootX - dz * width, 0, rootZ + dx * width, 0, 0,
        rootX + dx * lean, height, rootZ + dz * lean, 0, 0,
      );
      mesh.triangles.push(first, first + 1, first + 2, material + blade, -1);
    }
  }
}

async function meshData(
  meshes,
  palette,
  { spatial = false, glassIds = new Map(), captureShot = false, captureDisabled = false } = {}
) {
  const result = { vertices: [], triangles: [] };
  const shot = { vertices: result.vertices, triangles: [] };
  const surfaceNames = [];
  const temporary = new Vector3();
  for (const mesh of meshes) {
    const positions = mesh.getVerticesData?.("position");
    const indices = mesh.getIndices?.();
    const enabled = mesh.isEnabled();
    if (!positions?.length || !indices?.length || (!enabled && !captureDisabled)) continue;
    const visible = mesh.isVisible && mesh.visibility > 0.005;
    const pickable = captureShot && mesh.isPickable && visible;
    if (!visible && !pickable) continue;
    const surface = pickable ? surfaceNames.push(mesh.name) - 1 : -1;
    const glass = glassIds.get(mesh) ?? -1;
    const matrix = mesh.computeWorldMatrix(true);
    const uv = mesh.getVerticesData("uv");
    const tex = mesh.material?.albedoTexture ?? mesh.material?.diffuseTexture;
    const texMatrix = tex?.getTextureMatrix?.()?.m;
    const base = result.vertices.length / 5;
    for (let i = 0; i < positions.length / 3; i++) {
      if (enabled) {
        Vector3.TransformCoordinatesFromFloatsToRef(
          positions[i * 3],
          positions[i * 3 + 1],
          positions[i * 3 + 2],
          matrix,
          temporary
        );
      } else {
        // Keep vertex correspondence while a mechanical part is pocketed.
        // A collapsed part also avoids visible triangles during the tween.
        temporary.set(matrix.m[12], matrix.m[13], matrix.m[14]);
      }
      const u = uv?.[i * 2] ?? 0,
        v = uv?.[i * 2 + 1] ?? 0;
      result.vertices.push(
        temporary.x,
        temporary.y,
        temporary.z,
        texMatrix ? texMatrix[0] * u + texMatrix[4] * v + texMatrix[8] : u,
        texMatrix ? texMatrix[1] * u + texMatrix[5] * v + texMatrix[9] : v
      );
    }

    const submeshes = mesh.subMeshes?.length ? mesh.subMeshes : [null];
    for (const sub of submeshes) {
      const mat = await palette.material(sub?.getMaterial() ?? mesh.material);
      const begin = sub?.indexStart ?? 0;
      const end = begin + (sub?.indexCount ?? indices.length);
      for (let q = begin; q + 2 < end; q += 3) {
        const a = indices[q] + base,
          b = indices[q + 1] + base,
          c = indices[q + 2] + base;
        if (visible) result.triangles.push(a, b, c, mat, glass);
        if (pickable) shot.triangles.push(a, b, c, surface, glass);
      }
    }
  }
  // The world is partitioned after its render-only foliage has been added.
  return captureShot ? { ...result, shot: partition(shot, 5, true), surfaceNames } : partition(result, 5, spatial);
}

function appendMesh(target, added) {
  const start = target.vertices.length / 5;
  target.vertices.push(...added.vertices);
  for (let i = 0; i < added.triangles.length; i += 5) {
    const [a, b, c, material, glass] = added.triangles.slice(i, i + 5);
    target.triangles.push(start + a, start + b, start + c, material, glass);
  }
  return partition(target, 5, false);
}

// The verified browser collision boxes are gameplay data. The independent
// infantry rig never reads them to build geometry or poses.
function collisionPose(clip, phase) {
  if (clip === 0) return duelHitboxes[0];
  const first = clip === 1 ? 1 : 7;
  const fractional = phase * 6;
  const sample = Math.floor(fractional);
  const weight = fractional - sample;
  const a = duelHitboxes[first + sample];
  if (!weight) return a;
  const b = duelHitboxes[first + (sample + 1) % 6];
  return a.map((box, i) => box.map((value, j) => value + (b[i][j] - value) * weight));
}

function readNavigation() {
  const navNodes = [];
  const navEdges = [];
  for (let i = 0; i < BotNav.count; i++) {
    const edges = BotNav.edges[i] ?? [];
    navNodes.push([BotNav.xs[i], 0, BotNav.zs[i], navEdges.length, edges.length, BotNav.cover[i], BotNav.walkable[i]]);
    for (let j = 0; j < edges.length; j++) navEdges.push([edges[j], BotNav.edgeCosts[i][j]]);
  }
  return { navNodes, navEdges };
}

function pcm(samples) {
  return Int16Array.from(samples, (sample) => Math.round(Math.max(-1, Math.min(1, sample)) * 32767));
}

async function synth(method, seconds, args = []) {
  const ctx = new OfflineAudioContext(1, Math.ceil(seconds * audioHz), audioHz);
  const sound = new AudioSynth();
  // Run the same synthesis nodes at the native mixer rate, without speakers.
  sound.getAudioContext = () => ctx;
  sound[method](...args);
  const rendered = (await ctx.startRendering()).getChannelData(0);
  return pcm(rendered);
}

async function voice(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Missing voice ${url}`);
  const copy = await response.arrayBuffer();
  const ctx = new OfflineAudioContext(1, 1, audioHz);
  const decoded = await ctx.decodeAudioData(copy);
  const mono = new Float32Array(decoded.length);
  for (let channel = 0; channel < decoded.numberOfChannels; channel++) {
    const channelData = decoded.getChannelData(channel);
    for (let i = 0; i < mono.length; i++) mono[i] += channelData[i] / decoded.numberOfChannels;
  }
  return pcm(mono);
}

function footstep() {
  const samples = new Float32Array(0.2 * audioHz);
  let low = 0;
  for (let i = 0; i < samples.length; i++) {
    const noise = Math.random() * 2 - 1;
    low = low * 0.9 + noise * 0.1;
    samples[i] = low * 0.52 * Math.exp((-i / audioHz) * 17);
  }
  return pcm(samples);
}

async function readAudio() {
  // In Sound enum order, including the retired rain slot for older readers.
  const entries = [
    ["playMp44ShootSound", 0.32],
    ["playShootSound", 0.57],
    ["playPistolShootSound", 0.3],
    ["playMagInSound", 0.24],
    ["playBoltCycleSound", 0.48],
    ["playDryFireSound", 0.15],
    ["footstep"],
    ["playBulletWhizSound", 0.13],
    ["playGlassBreakSound", 0.4],
    ["playHitSound", 0.17],
    ["playPlayerDeathSound", 0.85],
    ["playUavOnlineSound", 0.9],
    ["playLaptopOpenSound", 0.3],
    ["playExplosionSound", 1.24, [0]],
    ["setRotorVolume", 2, [0.75]],
    ["reservedRain"],
    ["playKillSound", 0.25, [false]],
    ["playKillSound", 0.38, [true]],
  ];
  const result = [];
  for (const [name, seconds, args] of entries) {
    const samples =
      name === "reservedRain" ? new Int16Array(1) : name === "footstep" ? footstep() : await synth(name, seconds, args);
    result.push({ name, samples: base64(samples) });
  }
  return result;
}

async function readVoices() {
  const sources = import.meta.glob("../reference/browser/src/assets/voice/*/*.{wav,mp3}", { eager: true, query: "?url", import: "default" });
  const banks = [];
  for (const rival of ["ipwndurmom", "hanz"]) {
    for (const event of ["match-start", "taunt-kill", "taunt-victory"]) {
      const names = Object.keys(sources)
        .filter((path) => path.includes(`/voice/${rival}/${event}-`))
        .sort();
      if (!names.length || names.length > 16) throw new Error(`Invalid voice bank ${rival}/${event}`);
      const clips = await Promise.all(
        names.map(async (path) => ({
          name: path.split("/voice/")[1],
          samples: base64(await voice(sources[path])),
        }))
      );
      banks.push({ rival, event, clips });
    }
  }
  return banks;
}

// The browser already paints the rain clouds. Resample that panorama onto
// a cube so the native renderer can use its normal triangle projection.
function readSky(scene) {
  const panorama = scene.getMeshByName("skyDome")?.material?.emissiveTexture;
  if (!panorama) throw new Error("Ship Box sky texture is missing.");
  const { width, height } = panorama.getSize();
  const source = panorama.getContext().getImageData(0, 0, width, height).data;
  const size = 64;
  const directions = [
    (u, v) => [u, v, 1],
    (u, v) => [-u, v, -1],
    (u, v) => [1, v, -u],
    (u, v) => [-1, v, u],
    (u, v) => [u, 1, -v],
    (u, v) => [u, -1, v],
  ];
  const wrap = (x) => (x + width) % width;
  function sample(u, v, channel) {
    const x0 = Math.floor(u),
      y0 = Math.max(0, Math.min(height - 1, Math.floor(v)));
    const x1 = wrap(x0 + 1),
      y1 = Math.min(height - 1, y0 + 1);
    const x = u - x0,
      y = Math.max(0, Math.min(1, v - y0));
    const at = (sx, sy) => source[(sy * width + sx) * 4 + channel];
    return (at(wrap(x0), y0) * (1 - x) + at(x1, y0) * x) * (1 - y) + (at(wrap(x0), y1) * (1 - x) + at(x1, y1) * x) * y;
  }
  return directions.map((direction) => {
    const pixels = new Uint8Array(size * size);
    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        const [dx, dy, dz] = direction((x * 2) / (size - 1) - 1, (y * 2) / (size - 1) - 1);
        const u = ((Math.atan2(dx, dz) / (2 * Math.PI) + 1) % 1) * width;
        const v = (0.5 + Math.atan2(dy, Math.hypot(dx, dz)) / Math.PI) * height;
        pixels[y * size + x] = Math.round(sample(u, v, 1) * 0.83);
      }
    }
    return base64(pixels);
  });
}

export async function captureAssets({ seed, maxTexture, walkFrames, runFrames }) {
  Math.random = seededRandom(seed);
  const canvas = document.createElement("canvas");
  canvas.width = 800;
  canvas.height = 450;
  document.body.append(canvas);
  const engine = new Engine(canvas, false, { stencil: false });
  const scene = new Scene(engine);
  const camera = new CameraRig(scene);
  // An identity camera makes every ViewModelRig mesh camera-local.
  camera.camera.position.setAll(0);
  camera.camera.getViewMatrix(true);
  scene.activeCamera = camera.camera;

  try {
    const palette = new Palette(maxTexture);
    new ShipBoxMap(scene, new WorldMaterials(scene));
    BotNav.build();
    const glass = new Map();
    const glassHits = [];
    for (const mesh of scene.meshes) {
      if (mesh.metadata?.type === "carGlass") {
        glass.set(mesh, glassHits.length);
        glassHits.push(mesh.metadata.instance.panes.get(mesh).crackTex ? 3 : 1);
      }
    }
    const grass = scene.meshes.find((mesh) => mesh.hasThinInstances && mesh.material?.name === "grassTuftMat");
    const world = await meshData(
      scene.meshes.filter((m) => m.name !== "skyDome" && !m.hasThinInstances),
      palette,
      { spatial: true, glassIds: glass, captureShot: true }
    );
    for (const mesh of scene.meshes) mesh.setEnabled(false);
    window.assetProgress = `Ship Box base mesh: ${world.triangles.length / 5} visual triangles, ${world.shot.triangles.length / 5} pickable triangles`;
    console.log(window.assetProgress);

    const rig = new ViewModelRig(scene, camera, false, buildLiteSniper);
    const input = new Input(canvas);
    const weapons = new WeaponManager();
    const infantry = registerInfantryMaterials(palette, to565, base64);
    await scene.whenReadyAsync();
    const apache = new Apache(scene);
    apache.root.setEnabled(true);
    const helicopter = await meshData(apache.root.getChildMeshes(), palette);
    const jetBuild = { jets: [] };
    Killstreaks.prototype.buildJets.call(jetBuild, scene);
    const firstJet = jetBuild.jets[0].root;
    firstJet.setEnabled(true);
    const jet = await meshData([firstJet], palette);
    firstJet.setEnabled(false);

    const viewModels = [];
    const viewModelsAds = [];
    const tracks = [];
    let boltMargin = Infinity, boltChecks = 0;
    const viewGeometry = async (weapon) => {
      const root = rig.shown.root;
      const work = weapon.id === "m40a3" ? rig.cycleLift : 0;
      const carry = weapon.id === "m40a3" && !weapon.isAiming ? 1 - work : 0;
      const position = root.position.clone(), rotation = root.rotation.clone();
      // Frame the firing hand above the ammo display in the hip pose.
      root.position.x -= .042 * carry;
      root.position.y += .027 * carry;
      root.position.z += .012 * carry;
      // Keep the working hand visible beside Lite's larger eyepiece.
      root.position.x -= .055 * work;
      root.position.z += .045 * work;
      root.rotation.y -= .09 * work;
      root.rotation.z += .12 * work;
      try {
        const geometry = await meshData(root.getChildMeshes(), palette, { captureDisabled: true });
        return appendMesh(geometry, buildViewArms(weapon, rig.shown, infantry.player, geometry));
      } finally {
        root.position.copyFrom(position);
        root.rotation.copyFrom(rotation);
      }
    };
    const reset = (weapon) => {
      weapon.state = "idle";
      weapon.timer = 0;
      weapon.clipAmmo = weapon.config.magSize;
      weapon.adsAnimator.reset();
      weapon.isAiming = false;
      for (let i = 0; i < 60; i++) rig.update(1 / 60, weapon, input, false);
    };
    for (const weapon of weapons.weapons) {
      reset(weapon);
      scene.render();
      viewModels.push(await viewGeometry(weapon));
      weapon.adsAnimator.update(3, true, true);
      weapon.isAiming = true;
      // The scope overlay hides the scoped rifle, so use its last visible ADS pose.
      const frames = weapon.adsAnimator.frames;
      const visibleFrame = frames.findLastIndex((frame) => frame.scopeOpacity < 0.6);
      weapon.adsAnimator.reset();
      weapon.adsAnimator.update(visibleFrame / 60, true, true);
      rig.update(0, weapon, input, false);
      scene.render();
      viewModelsAds.push(await viewGeometry(weapon));
    }
    // Capture the reference rig at the phase boundaries and between them.
    // The second feed starts and ends at the same press pose so it can loop.
    const { RELOAD_OPEN: open, RELOAD_PER_ROUND: feed, RELOAD_CLOSE: close } = BoltActionSniper;
    const phase = (start, duration, fractions) => fractions.map((fraction) => start + duration * fraction);
    const feedSamples = [0, .19, .38, .55, .72, .80, .88];
    const repeatStart = open + feed, closeStart = open + 2 * feed;
    const sniperReload = [
      ...phase(0, open, [0, .17, .35, .52, .70, .85]),
      ...phase(open, feed, feedSamples),
      ...phase(repeatStart, feed, feedSamples),
      ...phase(closeStart, close, [0, .14, .28, .415, .55, .675, .8, .9, 1]),
    ];
    const cycle = weapons.weapons[1].config.boltCycleDuration;
    const sniperBolt = [
      0, ...phase(.30, cycle - .60, [0, .09, .18, .28, .38, .47, .56, .65, .74, .87, 1]),
      cycle - .20, cycle - .10, cycle,
    ];
    const clips = [
      [0, false, false, [0, .20, .39, .52, .62, .77, .92, 1.10, 1.26]],
      [1, false, false, sniperReload],
      [2, false, false, [0, .15, .31, .42, .55, .75, .94, 1.16]],
      [1, false, true, sniperBolt],
      [0, true, false, [0, .20, .39, .52, .63, .92, 1.10, 1.23, 1.36, 1.51, 1.62]],
      [2, true, false, [0, .15, .31, .42, .55, .75, .94, 1.16]],
    ];
    for (const [id, empty, bolt, samples] of clips) {
      const weapon = weapons.weapons[id];
      reset(weapon);
      weapon.clipAmmo = empty ? 0 : weapon.config.magSize - (id === 1 ? 2 : 1);
      if (id === 2) weapon.slideLocked = empty;
      if (bolt) weapon.state = "cycling";
      else weapon.startReload();
      const duration = bolt ? weapon.config.boltCycleDuration : weapon.reloadTotal;
      let elapsed = 0;
      const track = [];
      let repeatPose;
      for (const target of samples) {
        do {
          const step = Math.min(1 / 60, target - elapsed);
          elapsed += step;
          weapon.timer = Math.max(0, duration - elapsed);
          if (id === 2 && empty && elapsed >= .94) weapon.slideLocked = false;
          rig.update(step, weapon, input, false);
          if (id === 1) {
            boltMargin = Math.min(boltMargin, sampleBoltClearance(rig.shown));
            boltChecks++;
          }
        } while (elapsed < target - 1e-6);
        // Match idle on both ends, including the rig's damped carry motion.
        const resting = id === 1 && (target === 0 || Math.abs(target - duration) < 1e-6);
        const mesh = resting ? viewModels[id] : id === 1 && !bolt && target === closeStart
          ? repeatPose : await viewGeometry(weapon);
        if (id === 1 && !bolt && target === repeatStart) repeatPose = mesh;
        track.push({ seconds: target, mesh });
      }
      tracks.push(track);
    }
    if (!boltChecks || !Number.isFinite(boltMargin) || boltMargin < .0005) {
      throw new Error(`Lite sniper bolt needs more stock clearance: ${(boltMargin * 1000).toFixed(2)} mm`);
    }
    console.log(`Lite sniper bolt: ${(boltMargin * 1000).toFixed(2)} mm minimum sampled margin across ${boltChecks} poses`);
    console.log(`Exported weapons and Apache (${helicopter.triangles.length / 5} triangles)`);

    const { shot, surfaceNames, ...worldMesh } = world;
    addGrassTufts(worldMesh, palette, grass?.thinInstanceGetWorldMatrices() ?? []);
    registerInfantryFaces(palette, infantry, to565, base64);
    const rivalFrames = [];
    const hanzFrames = [];
    const rivalHitboxes = [];
    for (const [clip, count] of [1, walkFrames, runFrames].entries()) {
      for (let i = 0; i < count; i++) {
        const phase = i / count;
        const gait = ["idle", "walk", "run"][clip];
        const body = buildFieldInfantry(gait, phase, infantry.rival, "rival");
        rivalFrames.push(partition(body, 5, false));
        hanzFrames.push(partition(buildFieldInfantry(gait, phase, infantry.hanz, "hanz"), 5, false));
        rivalHitboxes.push(collisionPose(clip, phase));
      }
    }
    if (rivalFrames.length !== 1 + walkFrames + runFrames || !rivalFrames[0]?.triangles.length) {
      throw new Error("The infantry builder did not produce all requested animation frames.");
    }
    console.log(`Exported ${rivalFrames.length} sampled rival poses (${rivalFrames[0].triangles.length / 5} triangles per pose)`);
    const audio = await readAudio();
    const voices = await readVoices();
    const sky = readSky(scene);
    const obstacles = PlayerController.getObstacles().map((o) => [o.cx, o.cz, o.hw, o.hd, o.minY, o.maxY, o.yaw, o.cos, o.sin]);
    const playerBody = partition(buildFieldInfantry("idle", 0, infantry.player, "player"), 5, false);
    return {
      meshes: [partition(worldMesh, 5, true), ...viewModels, helicopter, ...viewModelsAds, ...rivalFrames],
      materials: palette.materials,
      textures: palette.textures,
      shot,
      surfaceNames,
      obstacles,
      glassHits,
      rivalHitboxes,
      hanzFrames,
      playerBody,
      ...readNavigation(),
      audio,
      voices,
      sky,
      tracks,
      boltClearance: { samples: boltChecks, crossSectionMarginMm: boltMargin * 1000 },
      jet,
    };
  } finally {
    engine.dispose();
  }
}
