import type { Scene } from "@babylonjs/core";
import type { CameraRig } from "../player/CameraRig";
import type { Effects } from "../rendering/Effects";
import { BaseWeapon } from "./BaseWeapon";

type PistolReloadEvent = "magOut" | "magIn" | "slideRelease";

// Semi-auto .45 sidearm. No bolt cycle: the slide blows back on every shot
// and locks open on the last round; reload is a single mag swap.
export class Pistol extends BaseWeapon<PistolReloadEvent> {
  // Reload timeline (seconds) — ViewModelRig mirrors these to drive the
  // support hand / magazine animation, so keep both in sync.
  public static readonly RELOAD_TOTAL = 1.16;
  public static readonly MAG_OUT_AT = 0.15; // mag drops free of the grip
  public static readonly MAG_DOWN_AT = 0.41; // hand + spent mag reach the pouch
  public static readonly MAG_IN_AT = 0.75; // fresh mag seats (ammo refills here)
  public static readonly SLIDE_RELEASE_AT = 0.94; // slide slams home if it was locked

  public readonly id = "usp45" as const;
  protected readonly kickDecayRate = 14;

  // Slide blowback: spikes to 1 on each shot and snaps back; ViewModelRig
  // reads getSlideBack() which also holds the slide open while locked.
  private slideKick: number = 0;
  private slideLocked: boolean = false;

  constructor() {
    super("usp45");
  }

  // 0..1 — how far back the slide sits this frame
  public getSlideBack(): number {
    return this.slideLocked ? 1 : this.slideKick;
  }

  protected override onTick(deltaTime: number): void {
    this.slideKick *= Math.exp(-26 * deltaTime);
  }

  protected override onReloadEvent(kind: PistolReloadEvent, effects: Effects): void {
    if (kind === "magOut") {
      effects.playMagOutSound();
    } else if (kind === "magIn") {
      // One-piece mag swap: the whole load arrives at once
      this.refillMag();
      effects.playMagInSound();
    } else {
      this.slideLocked = false; // slide slams forward, chambering a round
      effects.playSlideReleaseSound();
    }
  }

  protected override onCancelReload(): void {
    // Holstered after the fresh mag seated but before the slide-release
    // event: the gun comes back up chambered, not pinned open over a full mag
    if (this.clipAmmo > 0) this.slideLocked = false;
  }

  // Respawn refill bypasses the reload timeline, so release the slide here
  public override onRefill(): void {
    super.onRefill();
    this.slideLocked = false;
    this.slideKick = 0;
  }

  protected fire(scene: Scene, cameraRig: CameraRig, effects: Effects): void {
    this.fireCooldown = this.config.fireInterval;
    this.slideLocked = false; // firing cycles the slide — it can't stay pinned
    this.slideKick = 1;
    if (this.clipAmmo - 1 === 0) {
      this.slideLocked = true; // slide locks open on the empty mag
      this.state = "empty";
    }

    effects.playPistolShootSound();
    this.fireShot(scene, cameraRig, effects, 0.6, 0.085, -0.11, 0, 0.7);

    if (this.clipAmmo === 0 && this.reserveAmmo > 0) {
      this.startReload();
    }
  }

  protected startReload(): void {
    if (this.reserveAmmo <= 0 || this.clipAmmo === this.config.magSize) return;

    this.state = "reloading";
    this.reloadTotal = Pistol.RELOAD_TOTAL;
    this.timer = this.reloadTotal;

    this.reloadEvents = [
      { time: Pistol.MAG_OUT_AT, kind: "magOut" },
      { time: Pistol.MAG_IN_AT, kind: "magIn" },
    ];
    if (this.slideLocked) {
      this.reloadEvents.push({ time: Pistol.SLIDE_RELEASE_AT, kind: "slideRelease" });
    }
  }
}
