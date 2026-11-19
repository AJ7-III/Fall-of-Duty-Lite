import type { Scene } from "@babylonjs/core";
import type { CameraRig } from "../player/CameraRig";
import type { Effects } from "../rendering/Effects";
import { BaseWeapon } from "./BaseWeapon";

type Mp44ReloadEvent = "magOut" | "magIn" | "charge";

// MP44/StG-44-style automatic rifle. It keeps the weapon loop fast and punchy:
// full-auto fire while held, magazine reload, and an empty-mag charging handle.
export class Mp44 extends BaseWeapon<Mp44ReloadEvent> {
  public static readonly RELOAD_TOTAL = 1.26;
  public static readonly EMPTY_RELOAD_TOTAL = 1.62;
  public static readonly MAG_OUT_AT = 0.2;
  public static readonly MAG_DOWN_AT = 0.52;
  public static readonly MAG_IN_AT = 0.92;
  public static readonly CHARGE_START_AT = 1.1;

  public readonly id = "mp44" as const;
  protected readonly kickDecayRate = 18;
  protected override readonly automatic = true;

  private boltKick: number = 0;
  private reloadStartedEmpty: boolean = false;

  constructor() {
    super("mp44");
  }

  public getBoltBack(): number {
    return this.boltKick;
  }

  public isEmptyReload(): boolean {
    return this.state === "reloading" && this.reloadStartedEmpty;
  }

  protected override onTick(deltaTime: number): void {
    this.boltKick *= Math.exp(-34 * deltaTime);
  }

  protected override onReloadEvent(kind: Mp44ReloadEvent, effects: Effects): void {
    if (kind === "magOut") {
      effects.playMagOutSound();
    } else if (kind === "magIn") {
      this.refillMag();
      effects.playMagInSound();
    } else {
      effects.playBoltCycleSound();
    }
  }

  protected override onReloadComplete(): void {
    this.reloadStartedEmpty = false;
  }

  protected override onCancelReload(): void {
    this.reloadStartedEmpty = false;
  }

  public override onRefill(): void {
    super.onRefill();
    this.boltKick = 0;
    this.reloadStartedEmpty = false;
  }

  protected fire(scene: Scene, cameraRig: CameraRig, effects: Effects): void {
    this.fireCooldown = this.config.fireInterval;
    this.boltKick = 1;
    if (this.clipAmmo - 1 === 0) {
      this.state = "empty";
    }

    effects.playMp44ShootSound();

    // At full ADS the muzzle sits 0.05 below the sight line (barrel y=0.121
    // vs sight axis y=0.17); track it so the flash blooms off the barrel,
    // not over the sight picture, and shrink it so the post stays readable.
    const adsProgress = this.adsAnimator.getProgress();
    this.fireShot(scene, cameraRig, effects, 0.86, 0.06, -0.1, -0.05, 0.92 - 0.34 * adsProgress);

    if (this.clipAmmo === 0 && this.reserveAmmo > 0) {
      this.startReload();
    }
  }

  protected startReload(): void {
    if (this.reserveAmmo <= 0 || this.clipAmmo === this.config.magSize) return;

    this.reloadStartedEmpty = this.clipAmmo === 0;
    this.reloadTotal = this.reloadStartedEmpty ? Mp44.EMPTY_RELOAD_TOTAL : Mp44.RELOAD_TOTAL;
    this.state = "reloading";
    this.timer = this.reloadTotal;

    this.reloadEvents = [
      { time: Mp44.MAG_OUT_AT, kind: "magOut" },
      { time: Mp44.MAG_IN_AT, kind: "magIn" },
    ];
    if (this.reloadStartedEmpty) {
      this.reloadEvents.push({ time: Mp44.CHARGE_START_AT, kind: "charge" });
    }
  }
}
