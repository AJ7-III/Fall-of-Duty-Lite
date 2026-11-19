import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import type { Scene } from "@babylonjs/core";
import type { PlayerController } from "../player/PlayerController";
import { BotNav } from "./BotNav";
import { Bot } from "./Bot";
import type { BotContext } from "./Bot";
import { BOT_WEAPONS, TERMINATOR_LEVEL, applyDifficultyLevel, difficultyForLevel } from "./BotConfig";
import type { BotDifficulty } from "./BotConfig";
import type { Combatant } from "./BotCombat";
import type { Effects } from "../rendering/Effects";
import { MatchEvents } from "../ui/MatchEvents";
import { RIVALS, getRivals } from "../ui/MatchNames";
import type { RivalId } from "../ui/MatchNames";

// Free-for-all: lets one rival's soldier fight the other's with exactly the
// code it uses on the player. The bot AI only reads these fields.
export class BotTarget implements Combatant {
  public readonly eyeHeight = 1.62;
  public readonly isCrouching = false;
  public readonly isProne = false;
  public readonly isSprinting = false;
  public readonly bot: Bot;
  constructor(bot: Bot) {
    this.bot = bot;
  }
  get position(): Vector3 {
    return this.bot.position;
  }
  get velocity(): Vector3 {
    return this.bot.velocity;
  }
  get isDead(): boolean {
    return this.bot.dead;
  }
  takeDamage(amount: number, _fromPos: Vector3, attacker?: unknown): boolean {
    if (this.bot.dead) return false;
    this.bot.takeDamage(amount, "rival", attacker instanceof Bot ? attacker : null);
    return true;
  }
}

// Owns the enemy side: builds the nav graph from the map's collision data,
// spawns the opposition, runs their updates, keeps bodies apart, respawns
// the fallen out of the player's sight, and keeps the scoreboard.
// The yard is a duel by default — one enemy, one-on-one. A free-for-all puts
// one soldier per rival in the yard, fighting the player and each other.
// Tunable from the address bar (?difficulty=recruit..veteran&bots=1..6) and
// live from the pause menu's 1-10 difficulty slider (persisted to localStorage).
export class BotManager {
  private static readonly RESPAWN_DELAY = 5;
  private static readonly DIFFICULTY_KEY = "fallOfDuty.difficulty";
  private static readonly URL_LEVELS: Record<string, number> = {
    recruit: 2,
    regular: 3,
    hardened: 5,
    veteran: 8,
    terminator: 10,
  };
  // Corner pockets + the two wall pass-through containers
  private static readonly SPAWNS: ReadonlyArray<[number, number]> = [
    [13.5, 13.5],
    [-13.5, 13.5],
    [13.5, -13.5],
    [-13.5, -13.5],
    [-14.7, 0.6],
    [14.7, 0.35],
  ];

  public readonly bots: Bot[] = []; // this match's soldiers (the array is kept; consumers read it live)
  public playerKills = 0;

  private now = 0;
  private ctx: BotContext; // duel: every soldier fights the player
  // Every soldier built, one per rival at least: a duel benches the extras.
  // Each has its own tuning object (its rival's edge on top of the slider),
  // which the slider writes through to retune the yard mid-match.
  private roster: Bot[] = [];
  private tunings: BotDifficulty[] = [];
  private standIns: BotTarget[] = [];
  private contexts: BotContext[] = []; // free-for-all: per-soldier target
  private retargetAt: number[] = [];
  private duelSize: number;
  private freeForAll = false;
  private difficultyLevel: number;
  private static readonly TMP_EYE_A = new Vector3();
  private static readonly TMP_EYE_B = new Vector3();

  constructor(scene: Scene, player: PlayerController, effects: Effects) {
    // The graph must be grown after the map registers its obstacles
    BotNav.build();

    const params = new URLSearchParams(location.search);
    const saved = BotManager.readSavedDifficulty();
    const urlLevel = BotManager.URL_LEVELS[params.get("difficulty") ?? ""];
    this.difficultyLevel = Math.max(1, Math.min(10, Number.isFinite(saved) ? saved : (urlLevel ?? 5)));
    this.duelSize = Math.max(1, Math.min(6, parseInt(params.get("bots") ?? "1", 10) || 1));

    // Hand out spawns farthest from the player first; every third bot
    // carries the bolt rifle so the yard has one patient threat
    const spawns = this.spawnsFarthestFrom(player);
    for (let i = 0; i < Math.max(this.duelSize, RIVALS.length); i++) {
      const [x, z] = spawns[i % spawns.length];
      const primary = i % 3 === 2 ? BOT_WEAPONS.m40a3 : BOT_WEAPONS.mp44;
      const tuning = difficultyForLevel(this.difficultyLevel);
      const bot = new Bot(scene, tuning, primary, x, z, i);
      bot.reset(x, z, Math.atan2(-x, -z)); // face the center cross
      bot.setActive(i < this.duelSize);
      if (i < this.duelSize) this.bots.push(bot);
      this.roster.push(bot);
      this.tunings.push(tuning);
      this.standIns.push(new BotTarget(bot));
      this.contexts.push({ dt: 0, now: 0, target: player, human: player, effects, playerFired: false });
      this.retargetAt.push(0);
    }
    this.syncTerminator();

    this.ctx = { dt: 0, now: 0, target: player, human: player, effects, playerFired: false };
  }

  // This match's score for a rival (free-for-all: kills of the player and of the other rival)
  public rivalScore(id: RivalId): number {
    let kills = 0;
    for (const bot of this.bots) if (bot.rival.id === id) kills += bot.kills;
    return kills;
  }

  private applyTunings(): void {
    this.roster.forEach((bot, i) => applyDifficultyLevel(this.tunings[i], this.difficultyLevel, bot.rival.edge));
  }

  public getDifficultyLevel(): number {
    return this.difficultyLevel;
  }

  public setDifficultyLevel(level: number): void {
    this.difficultyLevel = Math.max(1, Math.min(10, Math.round(level)));
    this.applyTunings();
    this.syncTerminator();
    try {
      localStorage.setItem(BotManager.DIFFICULTY_KEY, this.difficultyLevel.toString());
    } catch {
      // private browsing — the slider still works for this session
    }
  }

  private static readSavedDifficulty(): number {
    try {
      return parseInt(localStorage.getItem(BotManager.DIFFICULTY_KEY) ?? "", 10);
    } catch {
      return NaN;
    }
  }

  // The liquid-metal skin follows the slider live: cross the Terminator
  // threshold and the whole fleet chromes mid-match; drop below and the
  // cloth comes back. (The matching health bonus lands on each respawn.)
  private syncTerminator(): void {
    const on = this.difficultyLevel >= TERMINATOR_LEVEL;
    for (const bot of this.roster) bot.setTerminator(on);
  }

  private spawnsFarthestFrom(player: PlayerController): Array<[number, number]> {
    return [...BotManager.SPAWNS].sort((a, b) => {
      const da = (a[0] - player.position.x) ** 2 + (a[1] - player.position.z) ** 2;
      const db = (b[0] - player.position.x) ** 2 + (b[1] - player.position.z) ** 2;
      return db - da;
    });
  }

  // Fresh round: zero the board and walk every bot back to a far spawn
  public resetMatch(player: PlayerController): void {
    this.playerKills = 0;
    // This match's line-up: a duel fields the rival's soldier(s); a
    // free-for-all one soldier per rival. Benched soldiers sit out hidden.
    const rivals = getRivals();
    this.freeForAll = rivals.length > 1;
    const size = this.freeForAll ? rivals.length : this.duelSize;
    this.bots.length = 0;
    this.roster.forEach((bot, i) => {
      bot.rival = this.freeForAll ? rivals[i % rivals.length] : rivals[0];
      bot.kills = 0;
      bot.setActive(i < size);
      if (i < size) this.bots.push(bot);
      this.contexts[i].target = player;
      this.retargetAt[i] = 0;
    });
    this.applyTunings(); // Hanz's soldiers carry his edge

    const spawns = this.spawnsFarthestFrom(player);
    this.bots.forEach((bot, i) => {
      const [x, z] = spawns[i % spawns.length];
      bot.respawnTimer = 0;
      bot.reset(x, z, Math.atan2(-x, -z));
    });
  }

  // The player-side mirror of respawn(): of the player's spawn corners, take
  // the one farthest from any living enemy, with a big bonus for being out
  // of their sight — never wake up staring down a muzzle.
  public pickPlayerSpawn(spawns: ReadonlyArray<[number, number]>): [number, number] {
    let best = spawns[0];
    let bestScore = -Infinity;
    for (const spawn of spawns) {
      let nearest = Infinity;
      let seen = false;
      BotManager.TMP_EYE_A.set(spawn[0], 1.62, spawn[1]);
      for (const bot of this.bots) {
        if (bot.dead) continue;
        const dx = spawn[0] - bot.position.x;
        const dz = spawn[1] - bot.position.z;
        nearest = Math.min(nearest, Math.sqrt(dx * dx + dz * dz));
        if (!seen) {
          BotManager.TMP_EYE_B.set(bot.position.x, bot.position.y + 1.62, bot.position.z);
          seen = !BotNav.losBlocked(BotManager.TMP_EYE_A, BotManager.TMP_EYE_B);
        }
      }
      const score = (nearest === Infinity ? 30 : Math.min(nearest, 30)) + (seen ? 0 : 24) + Math.random() * 2;
      if (score > bestScore) {
        bestScore = score;
        best = spawn;
      }
    }
    return best;
  }

  public update(dt: number, player: PlayerController, playerFired: boolean): void {
    this.now += dt;
    this.ctx.dt = dt;
    this.ctx.now = this.now;
    this.ctx.target = player;
    this.ctx.human = player;
    this.ctx.playerFired = playerFired;

    for (const bot of this.bots) {
      bot.sepX = 0;
      bot.sepZ = 0;
    }
    if (dt > 1e-4) {
      const invDt = 1 / dt;
      // Soft separation so two bots sharing a lane don't merge into one body,
      // injected as velocity so moveAndCollide resolves walls
      for (let i = 0; i < this.bots.length; i++) {
        const a = this.bots[i];
        if (a.dead) continue;
        for (let j = i + 1; j < this.bots.length; j++) {
          const b = this.bots[j];
          if (b.dead) continue;
          const dx = b.position.x - a.position.x;
          const dz = b.position.z - a.position.z;
          const d2 = dx * dx + dz * dz;
          if (d2 > 0.81 || d2 < 1e-6) continue;
          const d = Math.sqrt(d2);
          const pushVel = ((0.9 - d) * 0.5 * invDt) / d;
          const vx = dx * pushVel;
          const vz = dz * pushVel;
          a.sepX -= vx;
          a.sepZ -= vz;
          b.sepX += vx;
          b.sepZ += vz;
        }
      }
    }

    // Credit kills scored since the last frame (the player's shots, airstrikes,
    // the Apache) before any bot gets to kill the player. Otherwise a same-frame
    // trade is ordered by bot index: the death can zero the streak first and the
    // kill then lands after it, losing the UAV/airstrike/Apache reward.
    for (const bot of this.bots) {
      if (bot.dead && !bot.killCounted) this.registerKill(bot);
    }

    for (const bot of this.bots) {
      bot.update(this.freeForAll ? this.freeForAllContext(bot, player, playerFired) : this.ctx);
      if (bot.dead) {
        if (!bot.killCounted) this.registerKill(bot);
        else if ((bot.respawnTimer -= dt) <= 0) this.respawn(bot, player);
      }
    }
  }

  private registerKill(bot: Bot): void {
    bot.killCounted = true;
    bot.respawnTimer = BotManager.RESPAWN_DELAY;
    if (bot.lastDamageCause === "rival") {
      // the other rival's soldier got them: their point, not the player's
      const killer = bot.lastAttacker;
      if (killer) {
        killer.kills++;
        MatchEvents.emit("rivalKill", { killer: killer.rival.id, victim: bot.rival.id, weaponId: killer.weaponId });
      }
      return;
    }
    this.playerKills++;
    MatchEvents.emit("kill", { headshot: bot.lastHitZone >= 2, cause: bot.lastDamageCause, victim: bot.rival.id });
  }

  // Free-for-all: each soldier turns on whoever shot it in the last moment,
  // otherwise fights the nearest enemy it can see (the player or the other
  // rival's soldier). It re-picks only twice a second, and its current target
  // gets a 25% edge, so it commits to a fight instead of flickering between two.
  private freeForAllContext(bot: Bot, player: PlayerController, playerFired: boolean): BotContext {
    const slot = this.roster.indexOf(bot);
    const ctx = this.contexts[slot];
    ctx.dt = this.ctx.dt;
    ctx.now = this.now;
    ctx.human = player;
    const current = ctx.target;
    let next = current;
    if (current.isDead || this.now >= this.retargetAt[slot]) {
      this.retargetAt[slot] = this.now + 0.5;
      const recentlyHit = bot.lastHurtAt > this.now - 1.5;
      const attacker: Combatant | null = !recentlyHit
        ? null
        : bot.lastDamageCause === "rival"
          ? bot.lastAttacker && this.standIns[this.roster.indexOf(bot.lastAttacker)]
          : player; // the player's gun or streaks
      next = attacker && !attacker.isDead ? attacker : this.nearestEnemy(bot, player, current);
    }
    if (next !== current) {
      ctx.target = next;
      bot.retarget();
    }
    // hearing: the target's own gunfire this frame or last
    ctx.playerFired = next === player ? playerFired : (next as BotTarget).bot.lastShotAt >= this.now - this.ctx.dt;
    return ctx;
  }

  private nearestEnemy(bot: Bot, player: PlayerController, current: Combatant): Combatant {
    let best = current;
    let bestScore = Infinity;
    BotManager.TMP_EYE_A.set(bot.position.x, bot.position.y + 1.62, bot.position.z);
    const consider = (candidate: Combatant): void => {
      if (candidate.isDead) return;
      const dx = candidate.position.x - bot.position.x;
      const dz = candidate.position.z - bot.position.z;
      BotManager.TMP_EYE_B.set(candidate.position.x, candidate.position.y + candidate.eyeHeight, candidate.position.z);
      const seen = !BotNav.losBlocked(BotManager.TMP_EYE_A, BotManager.TMP_EYE_B);
      const score = Math.sqrt(dx * dx + dz * dz) * (seen ? 1 : 2.5) * (candidate === current ? 0.75 : 1);
      if (score < bestScore) {
        bestScore = score;
        best = candidate;
      }
    };
    consider(player);
    for (const other of this.bots) if (other !== bot) consider(this.standIns[this.roster.indexOf(other)]);
    return best;
  }

  // Back into the fight at the spawn the player is least likely to be
  // watching: farthest away, and hidden from their eyes if possible. The
  // jitter matters — without it a stationary player gets the SAME corner
  // every respawn, and "he always comes from the containers" is a pattern
  // good players farm. Hidden-ness still dominates; ties and near-ties vary.
  private respawn(bot: Bot, player: PlayerController): void {
    BotManager.TMP_EYE_B.set(player.position.x, player.position.y + player.eyeHeight, player.position.z);
    let best = BotManager.SPAWNS[0];
    let bestScore = -Infinity;
    for (const spawn of BotManager.SPAWNS) {
      const dx = spawn[0] - player.position.x;
      const dz = spawn[1] - player.position.z;
      BotManager.TMP_EYE_A.set(spawn[0], 1.62, spawn[1]);
      const hidden = BotNav.losBlocked(BotManager.TMP_EYE_A, BotManager.TMP_EYE_B);
      const score = Math.sqrt(dx * dx + dz * dz) + (hidden ? 30 : 0) + Math.random() * 11;
      if (score > bestScore) {
        bestScore = score;
        best = spawn;
      }
    }
    bot.reset(best[0], best[1], Math.atan2(-best[0], -best[1]));
    const slot = this.roster.indexOf(bot);
    if (this.freeForAll) {
      // back in hunting the player (the intuition below is about them); the
      // target picker takes over again shortly
      this.contexts[slot].target = player;
      this.retargetAt[slot] = this.now + 0.5;
    }

    // Spawn intuition: the dead remember the fight. Usually (always, at the
    // top of the scale) the respawned soldier re-enters hunting the player's
    // rough neighborhood instead of wandering at random — sharper minds get
    // a tighter fix. Aggression rises monotonically with the slider, so it
    // doubles as the skill proxy here.
    const skill = this.tunings[slot].aggression;
    if (!player.isDead && Math.random() < 0.45 + skill * 0.55) {
      bot.spawnIntuition(player, 2.5 + (1 - skill) * 9);
    }
  }
}
