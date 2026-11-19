import { Ray } from "@babylonjs/core/Culling/ray";
import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import type { Camera, Scene } from "@babylonjs/core";
import type { CarWreck } from "../world/CarWreck";
import type { Bot } from "../bots/Bot";
import type { ADSAnimator } from "./ADSAnimator";
import type { Input } from "../engine/Input";
import type { CameraRig } from "../player/CameraRig";
import type { Effects } from "../rendering/Effects";
import { SoldierBodyController } from "../bots/SoldierBody";

export type WeaponState = "idle" | "cycling" | "reloading" | "empty";

export type WeaponId = "m40a3" | "usp45" | "mp44";

// Common surface the game loop, HUD and viewmodel rig animate against —
// both the bolt rifle and the semi-auto pistol satisfy this.
export interface Weapon {
  readonly id: WeaponId;
  config: WeaponConfig;
  clipAmmo: number;
  reserveAmmo: number;
  state: WeaponState;
  timer: number;
  reloadTotal: number;
  adsAnimator: ADSAnimator;
  isAiming: boolean;
  visualKickZ: number;
  update(
    deltaTime: number,
    input: Input,
    playerController: unknown,
    cameraRig: CameraRig,
    scene: Scene,
    effects: Effects,
    inputEnabled: boolean
  ): void;
  // Holstering mid-reload abandons the animation but keeps rounds already loaded
  cancelReload(): void;
  // Respawn loadout refill — clear weapon state that assumes the previous
  // magazine's history (e.g. the pistol's locked-open slide)
  onRefill?(): void;
}

export interface WeaponConfig {
  name: string;
  magSize: number;
  maxReserveAmmo: number;
  damage: number; // per hit, before the struck bot part's zone multiplier
  fireInterval: number; // in seconds (for the rifle, gated by bolt cycle)
  boltCycleDuration: number;
  // Hipfire spread is not configured here: it is derived at fire time from the
  // on-screen crosshair size, so bullets always land inside the crosshair box.
  adsSpread: number;
  recoilHip: {
    pitch: number;
    yaw: number;
    kickBack: number;
  };
  recoilAds: {
    pitch: number;
    yaw: number;
    kickBack: number;
  };
}

export interface ADSFrame {
  frame: number;
  position: [number, number, number]; // x, y, z relative offsets
  rotation: [number, number, number]; // pitch, yaw, roll relative offsets (degrees)
  fov: number; // FOV in degrees (e.g. 70 down to 15)
  scopeOpacity: number; // 0 to 1
  sensitivityMultiplier: number; // 0 to 1
  vignetteOpacity: number; // 0 to 1
}

export interface ADSAnimationData {
  name: string;
  frames: ADSFrame[];
}

const CROSSHAIR_HALF_PX = 20; // matches #crosshair 40px CSS
const RAY_DIR = new Vector3();
const FALLBACK_NORMAL = new Vector3(0, 1, 0);
const HITSCAN_RAY = new Ray(new Vector3(), new Vector3(0, 0, 1), 1000);

let cachedCanvasHeight = 0;
let cachedCanvas: HTMLCanvasElement | null = null;
let resizeListening = false;

function getCanvasHeight(scene: Scene): number {
  const canvasEl = scene.getEngine().getRenderingCanvas();
  if (!resizeListening && typeof window !== "undefined") {
    resizeListening = true;
    window.addEventListener("resize", () => {
      cachedCanvasHeight = 0;
    });
  }
  if (cachedCanvasHeight <= 0 || canvasEl !== cachedCanvas) {
    cachedCanvas = canvasEl;
    const h = canvasEl?.clientHeight ?? 0;
    if (h > 0) cachedCanvasHeight = h;
    else return 720;
  }
  return cachedCanvasHeight;
}

// Shared fire() core for every weapon: cast one hitscan ray from the camera
// with the current spread, pick against the scene and apply the hit response.
// Weapon-specific pieces (ammo, fire-rate gating, sounds, muzzle flash,
// recoil) stay in each weapon class.
export function fireHitscanRay(
  scene: Scene,
  camera: Camera,
  effects: Effects,
  origin: Vector3,
  forward: Vector3,
  right: Vector3,
  up: Vector3,
  adsSpread: number,
  adsProgress: number,
  damage: number
): void {
  // ADS shots are precise; hip shots land anywhere inside the on-screen
  // crosshair box. Convert the crosshair's half-size in CSS px to a tangent
  // offset via the vertical FOV (px-to-angle is identical on both axes for a
  // vertical-fixed-FOV camera), then blend toward adsSpread as ADS completes.
  const screenHalfPx = getCanvasHeight(scene) / 2;
  const hipSpread = (CROSSHAIR_HALF_PX / screenHalfPx) * Math.tan(camera.fov / 2);
  const spread = hipSpread + (adsSpread - hipSpread) * adsProgress;

  RAY_DIR.copyFrom(forward);
  if (spread > 0.0001) {
    right.scaleAndAddToRef((Math.random() - 0.5) * 2 * spread, RAY_DIR);
    up.scaleAndAddToRef((Math.random() - 0.5) * 2 * spread, RAY_DIR);
    RAY_DIR.normalize();
  }

  HITSCAN_RAY.origin.copyFrom(origin);
  HITSCAN_RAY.direction.copyFrom(RAY_DIR);
  HITSCAN_RAY.length = 1000;
  SoldierBodyController.syncAllActiveHitboxes(scene);
  const hit = scene.pickWithRay(HITSCAN_RAY);

  if (hit && hit.hit && hit.pickedPoint) {
    const hitMesh = hit.pickedMesh;

    if (hitMesh && hitMesh.metadata && hitMesh.metadata.type === "bot") {
      // Enemy bot: each body part carries its own zone multiplier
      // (head 2.0 / torso 1.5 / limbs 1.0); a red, larger marker confirms kills
      const bot = hitMesh.metadata.instance as Bot;
      const zoneMult = hitMesh.metadata.zoneMult as number;
      bot.lastHitZone = zoneMult; // BotManager reads this to call headshots
      const killed = bot.takeDamage(damage * zoneMult);
      effects.showHitMarker(killed, killed && zoneMult >= 2);
    } else if (hitMesh && hitMesh.metadata && hitMesh.metadata.type === "carGlass") {
      // Car window: crack or shatter — never leave a floating decal on glass
      (hitMesh.metadata.instance as CarWreck).hitGlass(hitMesh, hit, effects);
    } else {
      // Static world geometry: permanent instanced bullet hole
      const hitNormal = hit.getNormal(true) || FALLBACK_NORMAL;
      effects.createBulletImpact(hit.pickedPoint, hitNormal);
    }
  }
}
