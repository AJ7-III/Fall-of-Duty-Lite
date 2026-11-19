import type { Material, Scene } from "@babylonjs/core";
import { Vector3, Quaternion, Matrix } from "@babylonjs/core/Maths/math.vector";
import { Mesh } from "@babylonjs/core/Meshes/mesh";
import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
// Thin instances live in a side-effect module since Babylon 9: without this
// import Mesh.thinInstanceSetBuffer is a silent no-op
import "@babylonjs/core/Meshes/thinInstanceMesh";
import { PBRMaterial } from "@babylonjs/core/Materials/PBR/pbrMaterial";
import { StandardMaterial } from "@babylonjs/core/Materials/standardMaterial";
import { Color3 } from "@babylonjs/core/Maths/math.color";
import { PointLight } from "@babylonjs/core/Lights/pointLight";
import { DynamicTexture } from "@babylonjs/core/Materials/Textures/dynamicTexture";
import { AudioSynth } from "./AudioSynth";

interface TransientSlot {
  mesh: Mesh;
  core?: Mesh;
  life: number;
}

interface FlashSlot extends TransientSlot {
  core: Mesh;
  scale: number;
}

interface SmokeSlot extends TransientSlot {
  max: number;
  vy: number;
  grow: number;
}

function stepLife(entry: TransientSlot, deltaTime: number, maxLife: number): number {
  if (entry.life <= 0) return 0;
  entry.life -= deltaTime;
  if (entry.life > 0) return entry.life / maxLife;
  entry.mesh.setEnabled(false);
  entry.core?.setEnabled(false);
  return 0;
}

// DynamicTexture defaults to CLAMP_ADDRESSMODE (unlike makeCanvasTexture's WRAP),
// which prevents edge bleeding on additive/alpha billboard sprites.
function makeCanvasTex(
  scene: Scene,
  name: string,
  draw: (ctx: CanvasRenderingContext2D) => void,
  hasAlpha = false
): DynamicTexture {
  const tex = new DynamicTexture(name, { width: 128, height: 128 }, scene, true);
  draw(tex.getContext() as CanvasRenderingContext2D);
  tex.update();
  if (hasAlpha) tex.hasAlpha = true;
  return tex;
}

function makeUnlitMat(
  scene: Scene,
  name: string,
  opts: {
    emissiveTex?: DynamicTexture;
    diffuseTex?: DynamicTexture;
    emissiveColor?: Color3;
    additive?: boolean;
  }
): StandardMaterial {
  const mat = new StandardMaterial(name, scene);
  if (opts.emissiveTex) mat.emissiveTexture = opts.emissiveTex;
  if (opts.diffuseTex) {
    mat.diffuseTexture = opts.diffuseTex;
    mat.useAlphaFromDiffuseTexture = true;
  }
  if (opts.emissiveColor) mat.emissiveColor = opts.emissiveColor;
  mat.diffuseColor = Color3.Black();
  mat.specularColor = Color3.Black();
  mat.disableLighting = true;
  mat.disableDepthWrite = true;
  if (opts.additive) {
    mat.alpha = 0.99; // engage blending; additive mode ignores the value
    mat.alphaMode = 1; // ALPHA_ADD: black pixels contribute nothing
  }
  return mat;
}

function makeBillboard(scene: Scene, name: string, size: number, mat: StandardMaterial): Mesh {
  const mesh = MeshBuilder.CreatePlane(name, { size }, scene);
  mesh.billboardMode = Mesh.BILLBOARDMODE_ALL;
  mesh.material = mat;
  mesh.isPickable = false;
  mesh.setEnabled(false);
  return mesh;
}

function makeBillboardPool<T>(
  scene: Scene,
  prefix: string,
  count: number,
  size: number,
  mat: StandardMaterial,
  wrap: (mesh: Mesh, index: number) => T
): T[] {
  const pool: T[] = [];
  for (let i = 0; i < count; i++) {
    pool.push(wrap(makeBillboard(scene, `${prefix}${i}`, size, mat), i));
  }
  return pool;
}

export class Effects extends AudioSynth {
  private scene: Scene;

  private hitMarkerEl: HTMLElement | null = null;
  private hitMarkerTimeout: ReturnType<typeof setTimeout> | null = null;

  // --- Pooled muzzle flashes ---
  // The light lives forever at intensity 0: adding/removing a light at
  // runtime changes every material's shader defines (recompile hitch on the
  // first shot, dirty-flag sweeps on every shot). Pooling keeps the scene's
  // light count constant. Each flash is two additive billboard quads
  // (star burst + hot core) animated per frame — they bloom outward and die
  // over ~85ms instead of blinking off. Only mesh-level properties
  // (scaling/visibility) and the light intensity animate, so the frozen
  // material never needs rebinding. A small round-robin pool lets the player
  // and several bots flash in the same instant; the single light rides the
  // newest flash (a second light would change every material's defines).
  private static readonly FLASH_POOL = 3;
  private static readonly FLASH_TIME = 0.085;
  private flashLight: PointLight;
  private flashes: FlashSlot[] = [];
  private flashCursor = 0;

  // --- Pooled tracers (incoming bot fire) ---
  // A thin additive cylinder stretched muzzle-to-impact, fading over ~80ms.
  // Hitscan means the whole streak exists at once; the fade sells the motion.
  private static readonly TRACER_POOL = 10;
  private static readonly TRACER_TIME = 0.08;
  private static readonly Y_AXIS = new Vector3(0, 1, 0);
  private tracers: TransientSlot[] = [];
  private tracerCursor = 0;
  private tmpDir = new Vector3();

  // --- Pooled explosions (airstrike bombs) ---
  // Same recipe as the muzzle flashes, scaled up: an additive fire-blob
  // billboard that blooms and dies in half a second, under a column of
  // alpha-blended smoke puffs that rise and fade. The explosion light is
  // created at startup (intensity 0) for the same reason the flash light is:
  // adding a light at runtime would recompile every frozen material.
  private static readonly FIRE_POOL = 5;
  private static readonly FIRE_TIME = 0.5;
  private static readonly SMOKE_POOL = 30; // a full airstrike keeps ~9 blasts x 3 puffs alive at once
  private explosionLight!: PointLight;
  private fireballs: TransientSlot[] = [];
  private fireCursor = 0;
  private smokes: SmokeSlot[] = [];
  private smokeCursor = 0;
  private readonly pools: readonly TransientSlot[][];

  // --- Persistent bullet holes (thin instances) ---
  // Every hole in the world is a thin instance of one flat disc: one mesh,
  // one material, one draw call no matter how many shots have landed, and
  // the holes never expire. Past MAX_HOLES the ring buffer recycles the
  // oldest hole, so memory stays bounded for arbitrarily long sessions.
  private static readonly MAX_HOLES = 512;
  private holeMesh: Mesh;
  private holeBuffer = new Float32Array(Effects.MAX_HOLES * 16);
  private holeCursor = 0;
  private holeCount = 0;

  // Scratch objects reused across shots — no per-shot allocations
  private static readonly DISC_AXIS = new Vector3(0, 0, 1);
  private static readonly UNIT_SCALE = new Vector3(1, 1, 1);
  private tmpQuat = new Quaternion();
  private tmpMat = new Matrix();
  private tmpPos = new Vector3();

  constructor(scene: Scene) {
    super();
    this.scene = scene;
    this.hitMarkerEl = document.getElementById("hit-marker");

    // Muzzle flash pool: one light + two billboard quads, parked until a shot
    this.flashLight = new PointLight("muzzleFlashLight", Vector3.Zero(), scene);
    this.flashLight.diffuse = new Color3(1, 0.7, 0.2);
    this.flashLight.intensity = 0;
    this.flashLight.range = 10;

    // Star-burst texture: hot white core, amber halo, eight tapered spikes.
    // Black background + additive blending means no alpha channel needed.
    const flashTex = makeCanvasTex(scene, "muzzleFlashTex", (fctx) => {
      fctx.fillStyle = "#000000";
      fctx.fillRect(0, 0, 128, 128);
      fctx.save();
      fctx.translate(64, 64);
      for (let i = 0; i < 8; i++) {
        const a = (Math.PI * 2 * i) / 8 + (i % 2) * 0.18;
        const len = i % 2 === 0 ? 58 : 36; // long/short alternating spikes
        fctx.save();
        fctx.rotate(a);
        const spike = fctx.createLinearGradient(0, 0, len, 0);
        spike.addColorStop(0, "rgba(255, 214, 130, 0.95)");
        spike.addColorStop(0.5, "rgba(255, 158, 64, 0.45)");
        spike.addColorStop(1, "rgba(120, 50, 10, 0)");
        fctx.fillStyle = spike;
        fctx.beginPath();
        fctx.moveTo(2, -4.5);
        fctx.lineTo(len, 0);
        fctx.lineTo(2, 4.5);
        fctx.closePath();
        fctx.fill();
        fctx.restore();
      }
      fctx.restore();
      const core = fctx.createRadialGradient(64, 64, 2, 64, 64, 26);
      core.addColorStop(0, "#fff8e6");
      core.addColorStop(0.4, "#ffd07a");
      core.addColorStop(1, "rgba(110, 45, 8, 0)");
      fctx.fillStyle = core;
      fctx.beginPath();
      fctx.arc(64, 64, 26, 0, Math.PI * 2);
      fctx.fill();
    });

    const flashMat = makeUnlitMat(scene, "flashMat", { emissiveTex: flashTex, additive: true });
    this.flashes = makeBillboardPool(scene, "muzzleFlashStar", Effects.FLASH_POOL, 0.55, flashMat, (mesh, i) => ({
      mesh,
      core: makeBillboard(scene, `muzzleFlashCore${i}`, 0.24, flashMat),
      life: 0,
      scale: 1,
    }));

    // Compile the flash shader now instead of stuttering on the first shot
    flashMat.forceCompilation(this.flashes[0].mesh);

    // Tracer pool: shared emissive material, one thin cylinder per slot
    const tracerMat = makeUnlitMat(scene, "tracerMat", {
      emissiveColor: new Color3(1.0, 0.83, 0.55),
      additive: true,
    });
    for (let i = 0; i < Effects.TRACER_POOL; i++) {
      const mesh = MeshBuilder.CreateCylinder(`tracer${i}`, { height: 1, diameter: 0.018, tessellation: 5 }, scene);
      mesh.material = tracerMat;
      mesh.isPickable = false;
      mesh.rotationQuaternion = new Quaternion();
      mesh.setEnabled(false);
      this.tracers.push({ mesh, life: 0 });
    }

    // Explosion pools: fire-blob billboards (additive, like the flashes) and
    // soft smoke puffs (alpha-blended), plus the always-resident light
    this.explosionLight = new PointLight("explosionLight", Vector3.Zero(), scene);
    this.explosionLight.diffuse = new Color3(1, 0.6, 0.25);
    this.explosionLight.intensity = 0;
    this.explosionLight.range = 22;

    const fireTex = makeCanvasTex(scene, "fireBlobTex", (fireCtx) => {
      fireCtx.fillStyle = "#000000";
      fireCtx.fillRect(0, 0, 128, 128);
      const blast = fireCtx.createRadialGradient(64, 64, 4, 64, 64, 60);
      blast.addColorStop(0, "#fff6df");
      blast.addColorStop(0.25, "#ffd27a");
      blast.addColorStop(0.55, "#ff7a26");
      blast.addColorStop(0.8, "rgba(140, 40, 8, 0.5)");
      blast.addColorStop(1, "rgba(0, 0, 0, 0)");
      fireCtx.fillStyle = blast;
      fireCtx.beginPath();
      fireCtx.arc(64, 64, 60, 0, Math.PI * 2);
      fireCtx.fill();
      // ragged hot lobes so the bloom isn't a perfect ball
      for (let i = 0; i < 7; i++) {
        const a = (Math.PI * 2 * i) / 7 + 0.4;
        const cx = 64 + Math.cos(a) * 30;
        const cy = 64 + Math.sin(a) * 30;
        const lobe = fireCtx.createRadialGradient(cx, cy, 2, cx, cy, 22);
        lobe.addColorStop(0, "rgba(255, 196, 110, 0.85)");
        lobe.addColorStop(1, "rgba(0, 0, 0, 0)");
        fireCtx.fillStyle = lobe;
        fireCtx.beginPath();
        fireCtx.arc(cx, cy, 22, 0, Math.PI * 2);
        fireCtx.fill();
      }
    });
    const fireMat = makeUnlitMat(scene, "fireBlobMat", { emissiveTex: fireTex, additive: true });
    this.fireballs = makeBillboardPool(scene, "fireball", Effects.FIRE_POOL, 1, fireMat, (mesh) => ({ mesh, life: 0 }));

    const smokeTex = makeCanvasTex(
      scene,
      "smokePuffTex",
      (smokeCtx) => {
        smokeCtx.clearRect(0, 0, 128, 128);
        for (const [px, py, r, a] of [
          [64, 64, 52, 0.5],
          [44, 50, 30, 0.4],
          [86, 56, 28, 0.4],
          [60, 86, 30, 0.35],
        ] as const) {
          const puff = smokeCtx.createRadialGradient(px, py, 2, px, py, r);
          puff.addColorStop(0, `rgba(70, 66, 60, ${a})`);
          puff.addColorStop(1, "rgba(70, 66, 60, 0)");
          smokeCtx.fillStyle = puff;
          smokeCtx.beginPath();
          smokeCtx.arc(px, py, r, 0, Math.PI * 2);
          smokeCtx.fill();
        }
      },
      true
    );
    const smokeMat = makeUnlitMat(scene, "smokePuffMat", {
      diffuseTex: smokeTex,
      emissiveColor: new Color3(0.32, 0.31, 0.29), // self-lit ash gray
    });
    this.smokes = makeBillboardPool(scene, "smokePuff", Effects.SMOKE_POOL, 1.6, smokeMat, (mesh) => ({
      mesh,
      life: 0,
      max: 1,
      vy: 1,
      grow: 1,
    }));
    this.pools = [this.flashes, this.tracers, this.fireballs, this.smokes];

    // Bullet-hole pool: a doubly-sided octagon disc matching the silhouette
    // of the old per-shot cylinder decal
    this.holeMesh = MeshBuilder.CreateDisc(
      "bulletHoles",
      { radius: 0.04, tessellation: 8, sideOrientation: Mesh.DOUBLESIDE },
      scene
    );
    this.holeMesh.isPickable = false; // follow-up shots must hit the surface, not old decals
    this.holeMesh.alwaysSelectAsActiveMesh = true; // skip culling: instance bounds are never recomputed
    this.holeMesh.receiveShadows = true;
    const holeMat = new PBRMaterial("impactMat", scene);
    holeMat.albedoColor = new Color3(0.06, 0.06, 0.06);
    holeMat.metallic = 0;
    holeMat.roughness = 0.95;
    holeMat.specularIntensity = 0.15;
    this.holeMesh.material = holeMat;
    this.holeMesh.thinInstanceSetBuffer("matrix", this.holeBuffer, 16, false);
    this.holeMesh.thinInstanceCount = 0;
  }

  public resetMatch(): void {
    this.holeCursor = 0;
    this.holeCount = 0;
    this.holeMesh.thinInstanceCount = 0;
    for (const pool of this.pools) {
      for (const e of pool) {
        e.life = 0;
        e.mesh.setEnabled(false);
        e.core?.setEnabled(false);
      }
    }
    this.flashLight.intensity = 0;
    this.explosionLight.intensity = 0;
    if (this.hitMarkerTimeout !== null) {
      clearTimeout(this.hitMarkerTimeout);
      this.hitMarkerTimeout = null;
    }
    if (this.hitMarkerEl) {
      this.hitMarkerEl.style.opacity = "0.0";
      this.hitMarkerEl.classList.remove("kill", "headshot");
    }
  }

  public dispose(): void {
    this.disposeAudio();
    if (this.hitMarkerTimeout !== null) clearTimeout(this.hitMarkerTimeout);
    this.hitMarkerTimeout = null;

    if (!this.scene.isDisposed) {
      this.flashLight.dispose();
      this.explosionLight.dispose();
      this.holeMesh.material?.dispose(false, true);
      this.holeMesh.dispose();
      const mats = new Set<Material>();
      for (const pool of this.pools) {
        for (const e of pool) {
          if (e.mesh.material) mats.add(e.mesh.material);
          if (e.core?.material) mats.add(e.core.material);
          e.mesh.dispose();
          e.core?.dispose();
        }
      }
      for (const mat of mats) mat.dispose(false, true);
    }
    for (const pool of this.pools) pool.length = 0;
  }

  // Visual Effects
  // scale: weapon-specific flash size (the pistol throws a smaller bloom)
  public createMuzzleFlash(barrelEndPosition: Vector3, scale: number = 1): void {
    const flash = this.flashes[this.flashCursor];
    this.flashCursor = (this.flashCursor + 1) % Effects.FLASH_POOL;
    flash.life = Effects.FLASH_TIME;
    flash.scale = scale * (0.85 + Math.random() * 0.35); // per-shot variance
    flash.mesh.position.copyFrom(barrelEndPosition);
    flash.core.position.copyFrom(barrelEndPosition);
    flash.mesh.setEnabled(true);
    flash.core.setEnabled(true);
    this.flashLight.position.copyFrom(barrelEndPosition); // light rides the newest flash
    this.updateTransients(0);
  }

  // Streak from a bot's muzzle to wherever the round stopped
  public createTracer(from: Vector3, to: Vector3): void {
    this.tmpDir.copyFrom(to).subtractInPlace(from);
    const length = this.tmpDir.length();
    if (length < 0.5) return;
    this.tmpDir.scaleInPlace(1 / length);

    const tracer = this.tracers[this.tracerCursor];
    this.tracerCursor = (this.tracerCursor + 1) % Effects.TRACER_POOL;
    Quaternion.FromUnitVectorsToRef(Effects.Y_AXIS, this.tmpDir, tracer.mesh.rotationQuaternion!);
    tracer.mesh.position.copyFrom(from).addInPlace(to).scaleInPlace(0.5);
    tracer.mesh.scaling.set(1, length, 1);
    tracer.mesh.visibility = 1;
    tracer.mesh.setEnabled(true);
    tracer.life = Effects.TRACER_TIME;
  }

  // Age prior effects at the start of each simulation frame.
  public updateTransients(deltaTime: number): void {
    let lightIntensity = 0;
    for (const flash of this.flashes) {
      const t = stepLife(flash, deltaTime, Effects.FLASH_TIME);
      if (t <= 0) continue;
      flash.mesh.scaling.setAll(flash.scale * (1 + (1 - t) * 0.75));
      flash.core.scaling.setAll(flash.scale * (0.7 + (1 - t) * 0.25));
      flash.mesh.visibility = t * t; // spikes die fast
      flash.core.visibility = Math.min(1, t * 1.5); // core lingers a touch
      lightIntensity = Math.max(lightIntensity, 3.4 * t * t);
    }
    this.flashLight.intensity = lightIntensity;

    for (const tracer of this.tracers) {
      const t = stepLife(tracer, deltaTime, Effects.TRACER_TIME);
      if (t > 0) tracer.mesh.visibility = t * 0.85;
    }

    // Explosions: the fire blob blooms hard and dies fast...
    let boomLight = 0;
    for (const fire of this.fireballs) {
      const t = stepLife(fire, deltaTime, Effects.FIRE_TIME);
      if (t <= 0) continue;
      fire.mesh.scaling.setAll(1.2 + (1 - t) * 5.4);
      fire.mesh.visibility = t * t;
      boomLight = Math.max(boomLight, 55 * t * t);
    }
    this.explosionLight.intensity = boomLight;

    // ...while the smoke column rises, swells and thins out above it
    for (const smoke of this.smokes) {
      const t = stepLife(smoke, deltaTime, smoke.max);
      if (t <= 0) continue;
      smoke.mesh.position.y += smoke.vy * deltaTime;
      smoke.mesh.scaling.setAll(1 + (1 - t) * smoke.grow);
      smoke.mesh.visibility = Math.min(1, t * 1.6) * 0.85;
    }
  }

  // One bomb going off: flash + fire blob + a short column of smoke.
  // Damage and camera shake are the caller's business — this is the show.
  public createExplosion(position: Vector3): void {
    const fire = this.fireballs[this.fireCursor];
    this.fireCursor = (this.fireCursor + 1) % Effects.FIRE_POOL;
    fire.life = Effects.FIRE_TIME;
    fire.mesh.position.copyFrom(position);
    fire.mesh.position.y += 0.9;
    fire.mesh.setEnabled(true);
    this.explosionLight.position.copyFrom(position);
    this.explosionLight.position.y += 1.5;

    for (let i = 0; i < 3; i++) {
      const smoke = this.smokes[this.smokeCursor];
      this.smokeCursor = (this.smokeCursor + 1) % Effects.SMOKE_POOL;
      smoke.max = 1.2 + Math.random() * 0.6;
      smoke.life = smoke.max;
      smoke.vy = 1.1 + Math.random() * 0.9;
      smoke.grow = 2.2 + Math.random() * 1.4;
      smoke.mesh.position.copyFrom(position);
      smoke.mesh.position.x += (Math.random() - 0.5) * 1.4;
      smoke.mesh.position.y += 0.6 + Math.random() * 0.8;
      smoke.mesh.position.z += (Math.random() - 0.5) * 1.4;
      smoke.mesh.scaling.setAll(1);
      smoke.mesh.setEnabled(true);
    }

    this.updateTransients(0);
  }

  public createBulletImpact(position: Vector3, normal: Vector3): void {
    const idx = this.holeCursor;
    this.holeCursor = (this.holeCursor + 1) % Effects.MAX_HOLES;
    this.holeCount = Math.min(this.holeCount + 1, Effects.MAX_HOLES);

    // Disc +z axis aligned to the hit normal, offset to prevent z-fighting
    this.tmpPos.copyFrom(position);
    normal.scaleAndAddToRef(0.01, this.tmpPos);
    Quaternion.FromUnitVectorsToRef(Effects.DISC_AXIS, normal, this.tmpQuat);
    Matrix.ComposeToRef(Effects.UNIT_SCALE, this.tmpQuat, this.tmpPos, this.tmpMat);
    this.tmpMat.copyToArray(this.holeBuffer, idx * 16);

    this.holeMesh.thinInstanceCount = this.holeCount;
    this.holeMesh.thinInstanceBufferUpdated("matrix");
  }

  // kill: a bigger marker held a beat longer for the Fall of Duty confirm.
  // headshot: the same swell in gold.
  public showHitMarker(kill: boolean = false, headshot: boolean = false): void {
    if (!this.hitMarkerEl) return;

    this.hitMarkerEl.classList.remove("hidden");
    this.hitMarkerEl.classList.toggle("kill", kill);
    this.hitMarkerEl.classList.toggle("headshot", headshot);
    this.hitMarkerEl.style.opacity = "1.0";

    // Play hit beep
    this.playHitSound();

    // One live timer: a stale fade from a previous rapid-fire hit must not
    // cut a fresh marker (especially a kill confirm) short
    if (this.hitMarkerTimeout !== null) clearTimeout(this.hitMarkerTimeout);
    this.hitMarkerTimeout = setTimeout(
      () => {
        this.hitMarkerTimeout = null;
        if (this.hitMarkerEl) {
          this.hitMarkerEl.style.opacity = "0.0";
        }
      },
      kill ? 320 : 150
    );
  }
}
