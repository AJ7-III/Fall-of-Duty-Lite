import type { Scene } from "@babylonjs/core";
import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import type { Input } from "../engine/Input";
import type { CameraRig } from "../player/CameraRig";
import type { Effects } from "../rendering/Effects";
import { ADSAnimator } from "./ADSAnimator";
import { weaponAdsFrames, weaponConfig } from "./WeaponData";
import { fireHitscanRay } from "./WeaponTypes";
import type { Weapon, WeaponConfig, WeaponId, WeaponState } from "./WeaponTypes";

export interface ReloadEvent<TKind extends string> {
  time: number;
  kind: TKind;
}

// Shared weapon state machine, ADS/FOV update, visual kickback decay,
// reload event scheduler, trigger/reload input handling, and zero-alloc
// hitscan + muzzle flash + camera recoil core.
export abstract class BaseWeapon<TReloadKind extends string = string> implements Weapon {
  public abstract readonly id: WeaponId;
  public config: WeaponConfig;
  public clipAmmo: number;
  public reserveAmmo: number;

  public state: WeaponState = "idle";
  public timer: number = 0;
  public reloadTotal: number = 0;

  public adsAnimator: ADSAnimator;
  public isAiming: boolean = false;
  public visualKickZ: number = 0;

  protected fireCooldown: number = 0;
  protected reloadEvents: ReloadEvent<TReloadKind>[] = [];

  protected abstract readonly kickDecayRate: number;
  protected readonly automatic: boolean = false;
  private automaticBurst = false;

  private static readonly FWD = Vector3.Forward();
  private static readonly RIGHT = Vector3.Right();
  private static readonly UP = Vector3.Up();
  private static readonly TMP_ORIGIN = new Vector3();
  private static readonly TMP_FORWARD = new Vector3();
  private static readonly TMP_RIGHT = new Vector3();
  private static readonly TMP_UP = new Vector3();
  private static readonly TMP_FLASH = new Vector3();

  constructor(id: WeaponId) {
    this.config = weaponConfig(id);
    this.clipAmmo = this.config.magSize;
    this.reserveAmmo = this.config.maxReserveAmmo;
    this.adsAnimator = new ADSAnimator(weaponAdsFrames(id));
  }

  public update(
    deltaTime: number,
    input: Input,
    _playerController: unknown,
    cameraRig: CameraRig,
    scene: Scene,
    effects: Effects,
    inputEnabled: boolean = true
  ): void {
    const canAds = this.state === "idle" || this.state === "empty";
    this.isAiming = inputEnabled && (input.isKeyDown("Space") || input.isMouseButtonDown(2)) && canAds;

    this.adsAnimator.update(deltaTime, this.isAiming, canAds);
    cameraRig.setFov(this.adsAnimator.getInterpolatedState().fov);

    this.visualKickZ *= Math.exp(-this.kickDecayRate * deltaTime);
    this.fireCooldown -= deltaTime;
    this.onTick(deltaTime);

    if (this.state === "cycling") {
      this.updateCycling(deltaTime, effects);
    } else if (this.state === "reloading") {
      this.timer -= deltaTime;
      const elapsed = this.reloadTotal - this.timer;

      while (this.reloadEvents.length > 0 && elapsed >= this.reloadEvents[0].time) {
        this.onReloadEvent(this.reloadEvents.shift()!.kind, effects);
      }

      if (this.timer <= 0) {
        this.reloadEvents = [];
        this.onReloadComplete();
        this.state = this.clipAmmo === 0 ? "empty" : "idle";
      }
    }

    const canUseTrigger = (this.state === "idle" || this.state === "empty") && inputEnabled;
    const pressed = canUseTrigger && input.isMouseButtonPressed(0);
    const held = canUseTrigger && this.automatic && input.isMouseButtonDown(0);
    if (!canAds || !held || pressed) this.automaticBurst = false;
    if (canUseTrigger && this.state === "empty" && this.clipAmmo === 0 && this.reserveAmmo > 0) {
      this.startReload();
      return;
    }

    if (canUseTrigger) {
      if (held || pressed) {
        if (this.clipAmmo > 0 && this.fireCooldown <= 0) {
          // Carry time past the previous shot deadline only within a held burst.
          const overshoot = this.automaticBurst ? this.fireCooldown : 0;
          this.fire(scene, cameraRig, effects);
          if (held) {
            this.fireCooldown = Math.max(0, this.fireCooldown + overshoot);
            this.automaticBurst = true;
          }
        } else if (this.clipAmmo === 0 && pressed) {
          if (this.reserveAmmo > 0) {
            this.startReload();
          } else {
            effects.playDryFireSound();
            if (this.automatic) this.fireCooldown = 0.2;
          }
        }
      }

      // fire() above can leave idle/empty (the bolt-action sniper starts cycling),
      // and a same-frame R must not overwrite that with a reload
      const stillReady = this.state === "idle" || this.state === "empty";
      if (stillReady && input.isKeyPressed("KeyR") && this.clipAmmo < this.config.magSize && this.reserveAmmo > 0) {
        this.startReload();
      }
    }
  }

  public cancelReload(): void {
    if (this.state !== "reloading") return;
    this.reloadEvents = [];
    this.timer = 0;
    this.state = this.clipAmmo === 0 ? "empty" : "idle";
    this.onCancelReload();
  }

  public onRefill(): void {
    this.fireCooldown = 0;
    this.automaticBurst = false;
  }

  protected fireShot(
    scene: Scene,
    cameraRig: CameraRig,
    effects: Effects,
    fwdDist: number,
    rightHip: number,
    upHip: number,
    upAds: number,
    flashScale?: number
  ): void {
    this.clipAmmo--;
    const recoilSettings = this.isAiming ? this.config.recoilAds : this.config.recoilHip;
    this.visualKickZ = recoilSettings.kickBack;

    const camera = cameraRig.camera;
    const forward = BaseWeapon.TMP_FORWARD;
    const right = BaseWeapon.TMP_RIGHT;
    const up = BaseWeapon.TMP_UP;
    camera.getDirectionToRef(BaseWeapon.FWD, forward);
    camera.getDirectionToRef(BaseWeapon.RIGHT, right);
    camera.getDirectionToRef(BaseWeapon.UP, up);
    // Read after the direction calls: Babylon only refreshes globalPosition when
    // the view matrix is recomputed, so before them it still holds the previous
    // frame's position and the ray and muzzle flash trail the camera by a step.
    const origin = BaseWeapon.TMP_ORIGIN.copyFrom(camera.globalPosition);

    const adsProgress = this.adsAnimator.getProgress();
    const hipOffset = 1 - adsProgress;
    const flashPos = BaseWeapon.TMP_FLASH.copyFrom(origin);
    forward.scaleAndAddToRef(fwdDist, flashPos);
    right.scaleAndAddToRef(rightHip * hipOffset, flashPos);
    up.scaleAndAddToRef(upHip * hipOffset + upAds * adsProgress, flashPos);
    effects.createMuzzleFlash(flashPos, flashScale);

    fireHitscanRay(scene, camera, effects, origin, forward, right, up, this.config.adsSpread, adsProgress, this.config.damage);

    cameraRig.applyRecoil(recoilSettings.pitch, recoilSettings.yaw, recoilSettings.kickBack);
  }

  protected refillMag(): void {
    const rounds = Math.min(this.config.magSize - this.clipAmmo, this.reserveAmmo);
    this.clipAmmo += rounds;
    this.reserveAmmo -= rounds;
  }

  protected onTick(_deltaTime: number): void {}
  protected updateCycling(_deltaTime: number, _effects: Effects): void {}
  protected onReloadComplete(): void {}
  protected onCancelReload(): void {}

  protected abstract fire(scene: Scene, cameraRig: CameraRig, effects: Effects): void;
  protected abstract startReload(): void;
  protected abstract onReloadEvent(kind: TReloadKind, effects: Effects): void;
}
