import type { Scene } from "@babylonjs/core";
import type { CameraRig } from "../player/CameraRig";
import type { Effects } from "../rendering/Effects";
import { BaseWeapon } from "./BaseWeapon";

type SniperReloadEvent = "boltOpen" | "insert" | "boltClose";

export class BoltActionSniper extends BaseWeapon<SniperReloadEvent> {
  public readonly id = "m40a3" as const;
  protected readonly kickDecayRate = 12;

  // Reload timeline (seconds): open the bolt, feed each round through the
  // open action into the internal magazine, then run the bolt home.
  // ViewModelRig reads these to drive the matching hand/bolt animation.
  public static readonly RELOAD_OPEN = 0.41;
  public static readonly RELOAD_PER_ROUND = 0.38;
  public static readonly RELOAD_CLOSE = 0.45;

  // Active reload round count (set on startReload)
  public reloadRounds: number = 0;

  // Track if bolt sound has been played during this cycle
  private boltSoundPlayed: boolean = false;

  constructor() {
    super("m40a3");
  }

  protected override updateCycling(deltaTime: number, effects: Effects): void {
    this.timer -= deltaTime;

    // Delay bolt cycle sound to play 0.4 seconds after the shot
    const elapsedCycleTime = this.config.boltCycleDuration - this.timer;
    if (elapsedCycleTime >= 0.4 && !this.boltSoundPlayed) {
      effects.playBoltCycleSound();
      this.boltSoundPlayed = true;
    }

    // Settle into idle/empty and let update() start the auto-reload once the
    // weapon is actually in use, so a bolt that finishes while the sniper is
    // lowering, the player is dead or the laptop is out doesn't begin a reload
    // nobody sees.
    if (this.timer <= 0) this.state = this.clipAmmo === 0 ? "empty" : "idle";
  }

  protected override onReloadEvent(kind: SniperReloadEvent, effects: Effects): void {
    if (kind === "boltOpen") {
      effects.playBoltOpenSound();
    } else if (kind === "insert") {
      if (this.reserveAmmo > 0 && this.clipAmmo < this.config.magSize) {
        this.clipAmmo++;
        this.reserveAmmo--;
      }
      effects.playRoundInsertSound();
    } else {
      effects.playBoltCloseSound();
    }
  }

  protected fire(scene: Scene, cameraRig: CameraRig, effects: Effects): void {
    this.state = "cycling";
    this.timer = this.config.boltCycleDuration;
    this.boltSoundPlayed = false;

    effects.playShootSound();
    this.fireShot(scene, cameraRig, effects, 0.8, 0.1, -0.145, 0);
  }

  protected startReload(): void {
    if (this.reserveAmmo <= 0 || this.clipAmmo === this.config.magSize) return;

    const rounds = Math.min(this.config.magSize - this.clipAmmo, this.reserveAmmo);
    this.reloadRounds = rounds;
    this.reloadTotal = BoltActionSniper.RELOAD_OPEN + rounds * BoltActionSniper.RELOAD_PER_ROUND + BoltActionSniper.RELOAD_CLOSE;

    this.state = "reloading";
    this.timer = this.reloadTotal;

    // Timeline events: bolt-open click mid-open, one insert per round timed
    // to the hand pressing it in, bolt-close clunk as the bolt slides home
    this.reloadEvents = [{ time: BoltActionSniper.RELOAD_OPEN * 0.55, kind: "boltOpen" }];
    for (let i = 0; i < rounds; i++) {
      this.reloadEvents.push({
        time: BoltActionSniper.RELOAD_OPEN + (i + 0.85) * BoltActionSniper.RELOAD_PER_ROUND,
        kind: "insert",
      });
    }
    this.reloadEvents.push({ time: this.reloadTotal - BoltActionSniper.RELOAD_CLOSE * 0.45, kind: "boltClose" });
  }
}
