import { TransformNode } from "@babylonjs/core/Meshes/transformNode";
import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import type { Scene, Mesh } from "@babylonjs/core";
import { BotNav } from "../bots/BotNav";
import type { CameraRig } from "../player/CameraRig";
import { BoltActionSniper } from "../weapons/BoltActionSniper";
import { Mp44 } from "../weapons/Mp44";
import { Pistol } from "../weapons/Pistol";
import type { Weapon, WeaponId } from "../weapons/WeaponTypes";
import type { Input } from "../engine/Input";
import { buildSniperViewModel } from "../viewmodels/SniperViewModel";
import { buildPistolViewModel } from "../viewmodels/PistolViewModel";
import { buildMp44ViewModel } from "../viewmodels/Mp44ViewModel";
import { ArmsRig } from "../viewmodels/ArmsRig";
import type { WeaponViewModel } from "../viewmodels/kit";

interface MagSwapConfig {
  magOutAt: number;
  magDownAt: number;
  magInAt: number;
  grab: Vector3;
  pouch: Vector3;
  magTravel: number;
  grabLift: number;
  holdLift: number;
  dropHideAt: number;
  insertShowAt: number;
  returnLift: number;
}

// The first-person presentation layer: places the active weapon under the
// camera (ADS keyframes + mouse sway + recoil + sprint carry + wall block +
// swap dip), drives each weapon's mechanical parts through its state
// machine, and moves the hand anchors the skinned arms chase — so a reload
// animation is "where do the hands go", and the arms follow.
export class ViewModelRig {
  private cameraRig: CameraRig;
  private models: Record<WeaponId, WeaponViewModel>;
  private shown: WeaponViewModel;
  private shownId: WeaponId = "m40a3";
  public readonly arms: ArmsRig;
  // Everything first-person hangs off this camera-parented pivot. It sits at
  // identity in play; dev tooling turns it to inspect the hands from the side.
  public readonly pivot: TransformNode;

  // Bolt-rifle pivots (rest offsets captured at build)
  private boltBasePos = new Vector3();
  // Pistol pivots
  private slideBasePos = new Vector3();
  private magBasePos = new Vector3();
  // MP44 pivots
  private mp44BoltBasePos = new Vector3();
  private mp44MagBasePos = new Vector3();

  // Pistol animation constants
  private static readonly SLIDE_TRAVEL = 0.03; // full blowback distance
  private static readonly HAMMER_REST_ROT = -0.55; // carried cocked
  private static readonly HAMMER_KICK_ROT = -0.35; // extra rock under blowback
  // mag exit direction: straight out of the raked grip (rake = +0.3 rad)
  private static readonly MAG_DIR = new Vector3(0, -0.955, -0.296);
  private static readonly MAG_TRAVEL = 0.45; // distance to fully off-hand
  // support-hand travel during the mag swap (offsets from its wrap grip)
  private static readonly HAND_GRAB = new Vector3(-0.005, -0.045, -0.015); // at the grip heel
  private static readonly HAND_POUCH = new Vector3(-0.05, -0.42, -0.2); // off-screen mag pouch
  private static readonly PISTOL_SWAP: MagSwapConfig = {
    magOutAt: Pistol.MAG_OUT_AT,
    magDownAt: Pistol.MAG_DOWN_AT,
    magInAt: Pistol.MAG_IN_AT,
    grab: ViewModelRig.HAND_GRAB,
    pouch: ViewModelRig.HAND_POUCH,
    magTravel: ViewModelRig.MAG_TRAVEL,
    grabLift: 0.75,
    holdLift: 0.75,
    dropHideAt: 0.8,
    insertShowAt: 0.2,
    returnLift: 0.75,
  };

  private static readonly MP44_BOLT_TRAVEL = 0.045;
  private static readonly MP44_MAG_DIR = new Vector3(0.025, -0.93, -0.32);
  private static readonly MP44_MAG_TRAVEL = 0.5;
  private static readonly MP44_HAND_GRAB = new Vector3(-0.052, -0.135, -0.06);
  private static readonly MP44_HAND_POUCH = new Vector3(-0.121, -0.493, -0.248);
  private static readonly MP44_CHARGE_HANDLE = new Vector3(-0.09, -0.008, -0.008);
  private static readonly MP44_SWAP: MagSwapConfig = {
    magOutAt: Mp44.MAG_OUT_AT,
    magDownAt: Mp44.MAG_DOWN_AT,
    magInAt: Mp44.MAG_IN_AT,
    grab: ViewModelRig.MP44_HAND_GRAB,
    pouch: ViewModelRig.MP44_HAND_POUCH,
    magTravel: ViewModelRig.MP44_MAG_TRAVEL,
    grabLift: 0.7,
    holdLift: 0.8,
    dropHideAt: 0.82,
    insertShowAt: 0.18,
    returnLift: 0.75,
  };

  // Bolt poses (rotation around the bolt axis + pull-back travel)
  private static readonly BOLT_REST_ROT = -0.9; // handle down-right
  private static readonly BOLT_OPEN_ROT = 0.15; // handle lifted
  private static readonly BOLT_PULL_Z = -0.085; // travel when cycling

  // Trigger-hand travel while working the bolt: the anchor's alternate pose
  // puts the hand on the handle; these are the extra offsets from there
  private static readonly ARM_POUCH = new Vector3(0.06, -0.34, -0.16); // down to the chest pouch (off screen)
  private static readonly ARM_PORT = new Vector3(0.015, 0.075, 0.0); // hovering over the open port

  // Death cam / killstreak laptop: the first-person weapon leaves the frame
  private hidden = false;

  // Sway values (mouse look lag)
  private swayX = 0;
  private swayY = 0;
  private swayRotX = 0;
  private swayRotY = 0;

  // 0..1 — how far the weapon is tilted inward so the hand work is on screen
  private cycleLift = 0;

  // 0..1 — how blocked the muzzle is by world geometry straight ahead
  private blockPull = 0;
  private static readonly BLOCK_RAY_RANGE = 1.15; // start reacting inside this distance
  private static readonly BLOCK_RAMP = 0.6; // fully blocked this far inside the range
  private static readonly FORWARD = Vector3.Forward();

  // Reused scratch vectors for wall-block OBB raycast & mag-swap hand lerp
  private static readonly TMP_RAY_ORIGIN = new Vector3();
  private static readonly TMP_RAY_DIR = new Vector3();
  private readonly magSwapOut = { armX: 0, armY: 0, armZ: 0, magDist: 0, magVisible: true, liftTarget: 0, tail: false };

  // Sprint carry: 0..1 pose blend + stride phase for the pumping motion
  private sprintBlend = 0;
  private sprintCycle = 0;

  constructor(scene: Scene, cameraRig: CameraRig, loadCharacter = true, makeSniper = buildSniperViewModel) {
    this.cameraRig = cameraRig;

    this.pivot = new TransformNode("viewmodelPivot", scene);
    this.pivot.parent = cameraRig.camera;
    this.models = {
      m40a3: makeSniper(scene),
      usp45: buildPistolViewModel(scene),
      mp44: buildMp44ViewModel(scene),
    };
    for (const vm of Object.values(this.models)) {
      vm.root.parent = this.pivot;
      vm.root.setEnabled(false);
    }
    this.shown = this.models.m40a3;
    this.shown.root.setEnabled(true);

    this.boltBasePos.copyFrom(this.models.m40a3.pivots.boltGroup.position);
    this.slideBasePos.copyFrom(this.models.usp45.pivots.slideGroup.position);
    this.magBasePos.copyFrom(this.models.usp45.pivots.magGroup.position);
    this.mp44BoltBasePos.copyFrom(this.models.mp44.pivots.boltGroup.position);
    this.mp44MagBasePos.copyFrom(this.models.mp44.pivots.magGroup.position);

    this.arms = new ArmsRig(scene, this.pivot, loadCharacter);
    this.arms.setAnchors(this.shown.hands.left, this.shown.hands.right);
  }

  private static smoothstep(t: number): number {
    const c = Math.max(0, Math.min(1, t));
    return c * c * (3 - 2 * c);
  }

  private get weaponMesh(): Mesh {
    return this.shown.root;
  }

  // Death cam and the killstreak laptop both take the weapon out of frame.
  // While hidden the rig is not updated, so re-showing re-syncs on the next
  // update call.
  public setHidden(hidden: boolean): void {
    if (this.hidden === hidden) return;
    this.hidden = hidden;
    this.weaponMesh.setEnabled(!hidden);
    this.arms.setHidden(hidden);
  }

  public update(
    deltaTime: number,
    activeWeapon: Weapon,
    input: Input,
    isSprinting: boolean,
    lowerAmount = 0,
    frameDeltaTime = deltaTime
  ): void {
    // 0. Weapon swap: enable the model that matches the active weapon
    if (activeWeapon.id !== this.shownId) {
      this.shownId = activeWeapon.id;
      this.shown.root.setEnabled(false);
      this.shown = this.models[activeWeapon.id];
      this.shown.root.setEnabled(!this.hidden);
      this.arms.setAnchors(this.shown.hands.left, this.shown.hands.right);
    }
    const weaponMesh = this.weaponMesh;

    const adsState = activeWeapon.adsAnimator.getInterpolatedState();
    const isPointerLocked = input.getIsPointerLocked();

    // 1. Weapon sway from mouse movement (reduced while aiming)
    let targetSwayX = 0;
    let targetSwayY = 0;
    let targetSwayRotX = 0;
    let targetSwayRotY = 0;
    if (isPointerLocked && frameDeltaTime > 0) {
      const mouseDelta = input.getMouseDelta();
      // The original tuning is in mouse counts per 60 Hz frame.
      const swayStrength = (activeWeapon.isAiming ? 0.02 : 1) / (60 * frameDeltaTime);
      targetSwayX = -mouseDelta.x * 0.00007 * swayStrength;
      targetSwayY = mouseDelta.y * 0.00007 * swayStrength;
      targetSwayRotX = mouseDelta.y * 0.0022 * swayStrength;
      targetSwayRotY = -mouseDelta.x * 0.0022 * swayStrength;
    }
    const swaySpeed = activeWeapon.isAiming ? 16 : 22;
    const swayFactor = 1 - Math.exp(-swaySpeed * deltaTime);
    this.swayX += (targetSwayX - this.swayX) * swayFactor;
    this.swayY += (targetSwayY - this.swayY) * swayFactor;
    this.swayRotX += (targetSwayRotX - this.swayRotX) * swayFactor;
    this.swayRotY += (targetSwayRotY - this.swayRotY) * swayFactor;

    // 2. Position = ADS keyframe + sway + recoil kickback
    const pos = adsState.position;
    weaponMesh.position.set(pos[0] + this.swayX, pos[1] + this.swayY, pos[2] - activeWeapon.visualKickZ);

    // 3. Rotation = ADS keyframe + sway + recoil pitch. Shorter weapons pivot
    // harder around the wrist; the MP44 sits between the pistol snap and the
    // sniper's slower shoulder recoil.
    const rot = adsState.rotation;
    const rotKickFactor = activeWeapon.id === "usp45" ? 1.5 : activeWeapon.id === "mp44" ? 0.78 : 0.4;
    const recoilRotKick = activeWeapon.visualKickZ * rotKickFactor;
    weaponMesh.rotation.x = (rot[0] * Math.PI) / 180 + this.swayRotX - recoilRotKick;
    weaponMesh.rotation.y = (rot[1] * Math.PI) / 180 + this.swayRotY;
    weaponMesh.rotation.z = (rot[2] * Math.PI) / 180;

    // Hide the weapon once the scope overlay dominates (sniper only — the
    // pistol's irons never raise a scope overlay)
    const scoped = adsState.scopeOpacity >= 0.6;
    weaponMesh.setEnabled(!scoped);
    this.arms.setHidden(this.hidden || scoped);

    // 4. Mechanical animation per weapon; returns how far to roll the
    // working side toward the camera
    let liftTarget = 0;
    if (activeWeapon.id === "m40a3") {
      liftTarget = this.updateSniperAnimation(deltaTime, activeWeapon as BoltActionSniper);
    } else if (activeWeapon.id === "mp44") {
      liftTarget = this.updateMp44Animation(deltaTime, activeWeapon as Mp44);
    } else {
      liftTarget = this.updatePistolAnimation(deltaTime, activeWeapon as Pistol);
    }

    // 5. Cycle lift: roll the weapon's working side toward the camera while
    // the hands are busy on it
    this.cycleLift += (liftTarget - this.cycleLift) * (1 - Math.exp(-10 * deltaTime));
    const lift = this.cycleLift;
    if (activeWeapon.id === "m40a3") {
      weaponMesh.position.x -= 0.05 * lift;
      weaponMesh.position.y += 0.02 * lift;
      weaponMesh.position.z -= 0.05 * lift;
      weaponMesh.rotation.y += 0.1 * lift; // bring the receiver toward center
      weaponMesh.rotation.z += 0.28 * lift; // roll the bolt handle up into view
      weaponMesh.rotation.x -= 0.03 * lift; // muzzle tips up slightly
    } else if (activeWeapon.id === "mp44") {
      weaponMesh.position.x -= 0.04 * lift;
      weaponMesh.position.y += 0.018 * lift;
      weaponMesh.position.z -= 0.045 * lift;
      weaponMesh.rotation.y += 0.16 * lift;
      weaponMesh.rotation.z += 0.18 * lift;
      weaponMesh.rotation.x -= 0.045 * lift;
    } else {
      weaponMesh.position.x -= 0.025 * lift;
      weaponMesh.position.y += 0.012 * lift;
      weaponMesh.position.z -= 0.03 * lift;
      weaponMesh.rotation.y += 0.14 * lift;
      weaponMesh.rotation.z += 0.2 * lift;
      weaponMesh.rotation.x -= 0.08 * lift;
    }

    // 6. Wall block: probe straight ahead against the OBB world; when geometry
    // sits inside the weapon's reach, pull it in and raise the muzzle so it
    // never pokes through obstacles (skipped at full ADS where blockTarget = 0)
    const adsProgress = activeWeapon.adsAnimator.getProgress();
    let blockTarget = 0;
    if (adsProgress < 0.99) {
      const camera = this.cameraRig.camera;
      // Reading the direction refreshes globalPosition for this frame.
      camera.getDirectionToRef(ViewModelRig.FORWARD, ViewModelRig.TMP_RAY_DIR);
      ViewModelRig.TMP_RAY_ORIGIN.copyFrom(camera.globalPosition);
      const hitDist = BotNav.rayHitWorld(ViewModelRig.TMP_RAY_ORIGIN, ViewModelRig.TMP_RAY_DIR, ViewModelRig.BLOCK_RAY_RANGE);
      if (hitDist < ViewModelRig.BLOCK_RAY_RANGE) {
        blockTarget = Math.min(1, (ViewModelRig.BLOCK_RAY_RANGE - hitDist) / ViewModelRig.BLOCK_RAMP);
      }
      if (activeWeapon.id === "usp45") {
        blockTarget = Math.max(0, blockTarget - 0.45) / 0.55; // short gun: only right in the face
      } else if (activeWeapon.id === "mp44") {
        blockTarget *= 0.9;
      }
      blockTarget *= 1 - adsProgress; // the scope owns the screen during ADS
    }
    this.blockPull += (blockTarget - this.blockPull) * (1 - Math.exp(-10 * deltaTime));
    const block = this.blockPull;
    weaponMesh.position.z -= 0.38 * block;
    weaponMesh.position.y -= 0.05 * block;
    weaponMesh.rotation.x -= 1.0 * block; // raise the muzzle skyward
    weaponMesh.rotation.y += 0.12 * block;

    // 7. Sprint carry: the weapon drops low across the body with the muzzle
    // swung up-left, pumping in rhythm with the stride. Yields to ADS and to
    // the wall-block pose so the layers never over-rotate.
    const sprintTarget = isSprinting ? 1 : 0;
    this.sprintBlend += (sprintTarget - this.sprintBlend) * (1 - Math.exp(-8 * deltaTime));
    const sprint = this.sprintBlend * (1 - this.blockPull) * (1 - adsProgress);
    if (sprint > 0.001) {
      this.sprintCycle += deltaTime * 18.6; // matches the sprint footstep cadence
      const pump = Math.sin(this.sprintCycle) * 0.012;
      const sway = Math.sin(this.sprintCycle * 0.5) * 0.02;
      weaponMesh.position.x += (-0.055 + sway) * sprint;
      weaponMesh.position.y += (-0.115 + pump) * sprint;
      weaponMesh.position.z -= 0.16 * sprint;
      weaponMesh.rotation.x -= (0.62 - pump * 1.5) * sprint;
      weaponMesh.rotation.y += 0.38 * sprint;
      weaponMesh.rotation.z += 0.3 * sprint;
    } else {
      this.sprintCycle = 0;
    }

    // 8. Weapon switch: the whole viewmodel dips below the screen edge while
    // the hands trade weapons, then rises with the next one
    if (lowerAmount > 0.001) {
      const drop = ViewModelRig.smoothstep(lowerAmount);
      weaponMesh.position.y -= 0.45 * drop;
      weaponMesh.position.z -= 0.08 * drop;
      weaponMesh.rotation.x -= 1.0 * drop;
      weaponMesh.rotation.z += 0.18 * drop;
    }

    // 9. The arms chase wherever the hands ended up this frame (skip while scoped)
    if (!scoped) {
      weaponMesh.computeWorldMatrix(true);
      this.arms.update();
    }
  }

  // --- Sniper: procedural bolt cycle + bolt-action reload. Returns the lift
  // target (how far the rifle rolls in toward the camera).
  private updateSniperAnimation(deltaTime: number, activeWeapon: BoltActionSniper): number {
    const vm = this.models.m40a3;
    const bolt = vm.pivots.boltGroup;
    const hand = vm.hands.right;

    // Targets computed per state, then either set directly (mid-cycle, already
    // continuous) or eased toward (state transitions) so nothing pops.
    let boltRot = ViewModelRig.BOLT_REST_ROT;
    let boltZ = 0;
    let grab = 0; // 0 = on the grip, 1 = on the bolt handle
    let armX = 0,
      armY = 0,
      armZ = 0; // extra travel beyond the handle pose
    let animatedDirectly = false;
    let liftTarget = 0;

    if (activeWeapon.state === "cycling") {
      const timer = activeWeapon.timer;
      const total = activeWeapon.config.boltCycleDuration;

      // Hand+bolt work spans from timer = total-0.3 down to 0.3 (0.6s window)
      const cycleStart = total - 0.3;
      const cycleEnd = 0.3;

      if (timer < cycleStart && timer > cycleEnd) {
        const p = (cycleStart - timer) / (cycleStart - cycleEnd); // 0 to 1
        const s = ViewModelRig.smoothstep;

        if (p < 0.18) {
          grab = s(p / 0.18); // reach up from the grip to the handle
        } else if (p < 0.38) {
          grab = 1; // rotate the bolt open
          const t = s((p - 0.18) / 0.2);
          boltRot = ViewModelRig.BOLT_REST_ROT + (ViewModelRig.BOLT_OPEN_ROT - ViewModelRig.BOLT_REST_ROT) * t;
        } else if (p < 0.56) {
          grab = 1; // pull back — ejects the spent case
          boltRot = ViewModelRig.BOLT_OPEN_ROT;
          boltZ = ViewModelRig.BOLT_PULL_Z * s((p - 0.38) / 0.18);
        } else if (p < 0.74) {
          grab = 1; // push forward — chambers the next round
          boltRot = ViewModelRig.BOLT_OPEN_ROT;
          boltZ = ViewModelRig.BOLT_PULL_Z * (1 - s((p - 0.56) / 0.18));
        } else {
          const t = s((p - 0.74) / 0.26); // close and return to the grip
          grab = 1 - t;
          boltRot = ViewModelRig.BOLT_OPEN_ROT + (ViewModelRig.BOLT_REST_ROT - ViewModelRig.BOLT_OPEN_ROT) * t;
        }
        armZ = boltZ * grab; // hand rides the bolt as it slides
        liftTarget = grab;
        animatedDirectly = true;
      }
    } else if (activeWeapon.state === "reloading") {
      // Full bolt-action reload: open the bolt, feed rounds one at a time
      // through the open port into the internal magazine, run the bolt home.
      // Mirrors the timeline in BoltActionSniper so sounds/ammo stay in sync.
      const tOpen = BoltActionSniper.RELOAD_OPEN;
      const tPer = BoltActionSniper.RELOAD_PER_ROUND;
      const tClose = BoltActionSniper.RELOAD_CLOSE;
      const elapsed = Math.max(0, activeWeapon.reloadTotal - activeWeapon.timer);
      const feedEnd = activeWeapon.reloadTotal - tClose;
      const s = ViewModelRig.smoothstep;
      const pouch = ViewModelRig.ARM_POUCH;
      const port = ViewModelRig.ARM_PORT;

      // Hand anchors relative to the handle pose: the pulled-back handle,
      // and the press-down point where the round gets pushed into the magazine
      const handleZ = ViewModelRig.BOLT_PULL_Z;
      const pressX = port.x;
      const pressY = port.y - 0.025;
      const pressZ = port.z + 0.045;

      if (elapsed < tOpen) {
        // Phase 1 — open the action: reach up, rotate the handle, draw back
        const p = elapsed / tOpen;
        grab = s(Math.min(1, p / 0.35));
        if (p >= 0.7) {
          boltRot = ViewModelRig.BOLT_OPEN_ROT;
          boltZ = ViewModelRig.BOLT_PULL_Z * s((p - 0.7) / 0.3);
          armZ = boltZ; // hand rides the bolt back
        } else if (p >= 0.35) {
          boltRot = ViewModelRig.BOLT_REST_ROT + (ViewModelRig.BOLT_OPEN_ROT - ViewModelRig.BOLT_REST_ROT) * s((p - 0.35) / 0.35);
        }
        liftTarget = grab;
      } else if (elapsed < feedEnd) {
        // Phase 2 — feed rounds: dip to the pouch, bring the round up to the
        // open port, press it down into the magazine; once per round
        boltRot = ViewModelRig.BOLT_OPEN_ROT;
        boltZ = ViewModelRig.BOLT_PULL_Z;
        grab = 1;
        liftTarget = 1;

        const sinceFeed = elapsed - tOpen;
        const round = Math.min(activeWeapon.reloadRounds - 1, Math.floor(sinceFeed / tPer));
        const t01 = (sinceFeed - round * tPer) / tPer;
        // First trip leaves from the bolt handle, later trips from the press point
        const fromX = round === 0 ? 0 : pressX;
        const fromY = round === 0 ? 0 : pressY;
        const fromZ = round === 0 ? handleZ : pressZ;

        if (t01 < 0.38) {
          const t = s(t01 / 0.38);
          armX = fromX + (pouch.x - fromX) * t;
          armY = fromY + (pouch.y - fromY) * t;
          armZ = fromZ + (pouch.z - fromZ) * t;
        } else if (t01 < 0.72) {
          const t = s((t01 - 0.38) / 0.34);
          armX = pouch.x + (port.x - pouch.x) * t;
          armY = pouch.y + (port.y - pouch.y) * t;
          armZ = pouch.z + (port.z - pouch.z) * t;
        } else if (t01 < 0.88) {
          const t = s((t01 - 0.72) / 0.16);
          armX = port.x + (pressX - port.x) * t;
          armY = port.y + (pressY - port.y) * t;
          armZ = port.z + (pressZ - port.z) * t;
        } else {
          armX = pressX;
          armY = pressY;
          armZ = pressZ;
        }
      } else {
        // Phase 3 — close the action: back to the handle, run the bolt
        // forward, lock the handle down, return to the grip
        const q = Math.min(1, (elapsed - feedEnd) / tClose);
        boltRot = ViewModelRig.BOLT_OPEN_ROT;
        boltZ = ViewModelRig.BOLT_PULL_Z;
        grab = 1;
        if (q < 0.28) {
          const t = s(q / 0.28);
          armX = pressX * (1 - t);
          armY = pressY * (1 - t);
          armZ = pressZ + (handleZ - pressZ) * t;
        } else if (q < 0.55) {
          boltZ = ViewModelRig.BOLT_PULL_Z * (1 - s((q - 0.28) / 0.27));
          armZ = boltZ;
        } else if (q < 0.8) {
          boltZ = 0;
          boltRot = ViewModelRig.BOLT_OPEN_ROT + (ViewModelRig.BOLT_REST_ROT - ViewModelRig.BOLT_OPEN_ROT) * s((q - 0.55) / 0.25);
        } else {
          boltZ = 0;
          boltRot = ViewModelRig.BOLT_REST_ROT;
          grab = 1 - s((q - 0.8) / 0.2);
        }
        liftTarget = q < 0.8 ? 1 : 1 - s((q - 0.8) / 0.2);
      }
      animatedDirectly = true;
    }

    if (animatedDirectly) {
      bolt.rotation.z = boltRot;
      bolt.position.set(this.boltBasePos.x, this.boltBasePos.y, this.boltBasePos.z + boltZ);
      hand.offset.set(armX, armY, armZ);
      hand.altBlend = grab;
    } else {
      // Ease toward the rest pose — smooth, no pops (covers cancelled reloads)
      const k = 1 - Math.exp(-14 * deltaTime);
      bolt.rotation.z += (boltRot - bolt.rotation.z) * k;
      bolt.position.z += (this.boltBasePos.z - bolt.position.z) * k;
      hand.offset.scaleInPlace(1 - k);
      hand.altBlend += (0 - hand.altBlend) * k;
    }

    return liftTarget;
  }

  private evalMagSwap(cfg: MagSwapConfig, elapsed: number, reloadTotal: number, customTail: boolean): void {
    const s = ViewModelRig.smoothstep;
    const out = this.magSwapOut;
    const { grab, pouch } = cfg;
    out.magDist = 0;
    out.magVisible = true;
    out.tail = false;

    if (elapsed < cfg.magOutAt) {
      const t = s(elapsed / cfg.magOutAt);
      out.armX = grab.x * t;
      out.armY = grab.y * t;
      out.armZ = grab.z * t;
      out.liftTarget = cfg.grabLift * t;
    } else if (elapsed < cfg.magDownAt) {
      const t = s((elapsed - cfg.magOutAt) / (cfg.magDownAt - cfg.magOutAt));
      out.armX = grab.x + (pouch.x - grab.x) * t;
      out.armY = grab.y + (pouch.y - grab.y) * t;
      out.armZ = grab.z + (pouch.z - grab.z) * t;
      out.magDist = cfg.magTravel * t;
      out.magVisible = t < cfg.dropHideAt;
      out.liftTarget = cfg.holdLift;
    } else if (elapsed < cfg.magInAt) {
      const t = s((elapsed - cfg.magDownAt) / (cfg.magInAt - cfg.magDownAt));
      out.armX = pouch.x + (grab.x - pouch.x) * t;
      out.armY = pouch.y + (grab.y - pouch.y) * t;
      out.armZ = pouch.z + (grab.z - pouch.z) * t;
      out.magDist = cfg.magTravel * (1 - t);
      out.magVisible = t > cfg.insertShowAt;
      out.liftTarget = cfg.holdLift;
    } else if (customTail) {
      out.tail = true;
    } else {
      const t = s((elapsed - cfg.magInAt) / (reloadTotal - cfg.magInAt));
      out.armX = grab.x * (1 - t);
      out.armY = grab.y * (1 - t);
      out.armZ = grab.z * (1 - t);
      out.liftTarget = cfg.returnLift * (1 - t);
    }
  }

  private resetMagSwapOut(): void {
    const out = this.magSwapOut;
    out.armX = out.armY = out.armZ = 0;
    out.magDist = 0;
    out.magVisible = true;
    out.liftTarget = 0;
  }

  private applyMagPlacement(mag: TransformNode, basePos: Vector3, dir: Vector3): void {
    const out = this.magSwapOut;
    mag.setEnabled(out.magVisible);
    mag.position.set(basePos.x + dir.x * out.magDist, basePos.y + dir.y * out.magDist, basePos.z + dir.z * out.magDist);
  }

  // --- MP44: reciprocating bolt/charging handle, curved-mag reload, support
  // hand leaving the fore-end to run the magazine and empty-reload charge.
  private updateMp44Animation(deltaTime: number, weapon: Mp44): number {
    const vm = this.models.mp44;
    const s = ViewModelRig.smoothstep;
    const out = this.magSwapOut;
    let boltBack = weapon.getBoltBack();
    let animatedDirectly = false;

    if (weapon.state === "reloading") {
      const elapsed = Math.max(0, weapon.reloadTotal - weapon.timer);
      animatedDirectly = true;
      this.evalMagSwap(ViewModelRig.MP44_SWAP, elapsed, weapon.reloadTotal, weapon.isEmptyReload());

      if (out.tail) {
        const grab = ViewModelRig.MP44_HAND_GRAB;
        const charge = ViewModelRig.MP44_CHARGE_HANDLE;
        const q = Math.max(0, (elapsed - Mp44.MAG_IN_AT) / (weapon.reloadTotal - Mp44.MAG_IN_AT));
        if (elapsed < Mp44.CHARGE_START_AT) {
          out.armX = grab.x;
          out.armY = grab.y;
          out.armZ = grab.z;
          out.liftTarget = 0.8;
        } else if (q < 0.34) {
          // the reach starts when the charge does, so it eases out of the grab pose
          const q0 = (Mp44.CHARGE_START_AT - Mp44.MAG_IN_AT) / (weapon.reloadTotal - Mp44.MAG_IN_AT);
          const t = s((q - q0) / (0.34 - q0));
          out.armX = grab.x + (charge.x - grab.x) * t;
          out.armY = grab.y + (charge.y - grab.y) * t;
          out.armZ = grab.z + (charge.z - grab.z) * t;
          out.liftTarget = 0.85;
        } else if (q < 0.62) {
          const t = s((q - 0.34) / 0.28);
          out.armX = charge.x - 0.015 * t;
          out.armY = charge.y;
          out.armZ = charge.z - 0.055 * t;
          boltBack = Math.max(boltBack, t);
          out.liftTarget = 0.9;
        } else if (q < 0.78) {
          const t = s((q - 0.62) / 0.16);
          out.armX = charge.x - 0.015 * (1 - t);
          out.armY = charge.y;
          out.armZ = charge.z - 0.055 * (1 - t);
          boltBack = Math.max(boltBack, 1 - t);
          out.liftTarget = 0.85;
        } else {
          const t = s((q - 0.78) / 0.22);
          out.armX = charge.x * (1 - t);
          out.armY = charge.y * (1 - t);
          out.armZ = charge.z * (1 - t);
          out.liftTarget = 0.85 * (1 - t);
        }
      }
    } else {
      this.resetMagSwapOut();
    }

    const bolt = vm.pivots.boltGroup;
    bolt.position.set(
      this.mp44BoltBasePos.x,
      this.mp44BoltBasePos.y,
      this.mp44BoltBasePos.z - ViewModelRig.MP44_BOLT_TRAVEL * boltBack
    );

    this.applyMagPlacement(vm.pivots.magGroup, this.mp44MagBasePos, ViewModelRig.MP44_MAG_DIR);

    this.settleHand(vm.hands.left.offset, animatedDirectly, out.armX, out.armY, out.armZ, deltaTime);
    return out.liftTarget;
  }

  // --- Pistol: slide blowback, hammer rock, and the mag-swap reload where
  // the support hand drops the spent mag and slaps a fresh one home.
  private updatePistolAnimation(deltaTime: number, weapon: Pistol): number {
    const vm = this.models.usp45;
    const out = this.magSwapOut;

    // Slide rides its blowback value every frame; the lock-back on an empty
    // mag and the slam home on slide release both come through getSlideBack()
    const slideBack = weapon.getSlideBack();
    vm.pivots.slideGroup.position.set(
      this.slideBasePos.x,
      this.slideBasePos.y,
      this.slideBasePos.z - ViewModelRig.SLIDE_TRAVEL * slideBack
    );
    vm.pivots.hammerGroup.rotation.x = ViewModelRig.HAMMER_REST_ROT + ViewModelRig.HAMMER_KICK_ROT * slideBack;

    let animatedDirectly = false;
    if (weapon.state === "reloading") {
      const elapsed = Math.max(0, weapon.reloadTotal - weapon.timer);
      animatedDirectly = true;
      this.evalMagSwap(ViewModelRig.PISTOL_SWAP, elapsed, weapon.reloadTotal, false);
    } else {
      this.resetMagSwapOut();
    }

    this.applyMagPlacement(vm.pivots.magGroup, this.magBasePos, ViewModelRig.MAG_DIR);

    this.settleHand(vm.hands.left.offset, animatedDirectly, out.armX, out.armY, out.armZ, deltaTime);
    return out.liftTarget;
  }

  // Hand offset: written directly while an animation owns it, eased back to
  // the grip otherwise (covers reload cancel on swap)
  private settleHand(offset: Vector3, direct: boolean, x: number, y: number, z: number, deltaTime: number): void {
    if (direct) {
      offset.set(x, y, z);
    } else {
      offset.scaleInPlace(Math.exp(-14 * deltaTime));
    }
  }
}
