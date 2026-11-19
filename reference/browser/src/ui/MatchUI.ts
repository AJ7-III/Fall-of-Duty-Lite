import type { Effects } from "../rendering/Effects";
import { MatchEvents } from "./MatchEvents";
import { TERMINATOR_LEVEL, difficultyLevelName } from "../bots/BotConfig";
import { StartCharacterPreview } from "./StartCharacterPreview";
import {
  getRivals,
  getUserDisplayName,
  isFreeForAll,
  normalizeUserDisplayName,
  rivalById,
  setUserDisplayName,
} from "./MatchNames";
import type { OpponentChoice, RivalId } from "./MatchNames";
import { Settings } from "./Settings";
import type { GraphicsQuality } from "./Settings";

// All the "game" around the gunplay: the YOU/OPFOR scoreboard, kill banners
// and floating score popups, killstreak callouts, the kill feed, the pause
// menu (with the 1-10 enemy difficulty slider) and the match-end screen.
// Listens on MatchEvents so gameplay code never touches the DOM.

export type MatchResult = "victory" | "defeat" | "draw";

export interface MatchUICallbacks {
  onStart: () => void;
  onResume: () => void;
  onEndMatch: () => void;
  onPlayAgain: () => void;
  onDifficultyChange: (level: number) => void;
  onToggleTrashTalk: (muted: boolean) => void;
  onGraphicsChange: (quality: GraphicsQuality) => void;
  onPreviewRival: (choice: OpponentChoice) => void;
}

// One rival's line on the scoreboard and end screen (one in a duel, two in a free-for-all)
export interface ScoreLine {
  readonly id: RivalId;
  readonly name: string;
  score: number;
}

const QUALITY_DESC: Record<GraphicsQuality, string> = {
  high: "Native resolution · 4× MSAA · ambient occlusion · sharpen",
  balanced: "1.5× resolution cap · 2× MSAA + FXAA · sharpen",
  performance: "60 FPS cap · lighter effects · reduced rain · lower power use",
};

const WEAPON_NAMES: Record<string, string> = {
  mp44: "MP44",
  usp45: "USP .45",
  m40a3: "M40A3",
  airstrike: "AIRSTRIKE",
  apache: "APACHE",
};

// Streak callouts — the Fall of Duty reward ladder (Killstreaks delivers the goods;
// these are the announcements): [streak length, label, fanfare tier]
const STREAK_CALLS: ReadonlyArray<[number, string, number]> = [
  [3, "UAV RECON ONLINE", 1],
  [5, "AIRSTRIKE READY — PRESS 4", 2],
  [7, "APACHE READY — PRESS 4", 3],
];

// End-screen copy and SVG emblems per outcome; emblems inherit currentColor.
const RESULT_META: Record<MatchResult, { emblem: string; title: string; subtitle: string; verdict: (rival: string) => string }> =
  {
    victory: {
      emblem: `<svg viewBox="0 0 64 64" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linejoin="round" stroke-linecap="round">
        <path d="M16 7 L22 22" opacity="0.85"/><path d="M48 7 L42 22" opacity="0.85"/>
        <circle cx="32" cy="34" r="18" fill="currentColor" fill-opacity="0.12"/>
        <path d="M32 21 l3.7 7.7 8.3 1.1 -6.1 5.8 1.5 8.3 -7.4 -4 -7.4 4 1.5 -8.3 -6.1 -5.8 8.3 -1.1 Z" fill="currentColor" fill-opacity="0.95" stroke="none"/>
      </svg>`,
      title: "VICTORY",
      subtitle: "ENEMY FORCE ELIMINATED",
      verdict: (rival) => `You outgunned ${rival}.`,
    },
    defeat: {
      emblem: `<svg viewBox="0 0 64 64" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linejoin="round" stroke-linecap="round">
        <circle cx="32" cy="32" r="19" fill="currentColor" fill-opacity="0.1"/>
        <path d="M32 13 v22" /><path d="M32 35 l-9 12" opacity="0.6"/><path d="M32 35 l9 12" opacity="0.6"/>
        <circle cx="32" cy="44" r="2.6" fill="currentColor" stroke="none"/>
      </svg>`,
      title: "DEFEATED",
      subtitle: "YOU WERE ELIMINATED",
      verdict: (rival) => `${rival} got the better of you.`,
    },
    draw: {
      emblem: `<svg viewBox="0 0 64 64" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round">
        <circle cx="32" cy="32" r="19" fill="currentColor" fill-opacity="0.08"/>
        <path d="M22 27 h20" /><path d="M22 37 h20" />
      </svg>`,
      title: "DRAW",
      subtitle: "STALEMATE — NO WINNER",
      verdict: () => "Neither side broke.",
    },
  };

export class MatchUI {
  private youEl = document.getElementById("sb-you");
  private enemyEl = document.getElementById("sb-enemy");
  private enemyLabelEl = document.getElementById("sb-enemy-label");
  private enemy2El = document.getElementById("sb-enemy2");
  private enemy2LabelEl = document.getElementById("sb-enemy2-label");
  private scoreboardEl = document.getElementById("scoreboard");
  private feedEl = document.getElementById("killfeed");
  private bannerEl = document.getElementById("kill-banner");
  private bannerTitleEl = document.getElementById("kill-banner-title");
  private bannerSubEl = document.getElementById("kill-banner-sub");
  private streakEl = document.getElementById("streak-banner");
  private pointsEl = document.getElementById("points-layer");

  private hudRootEl = document.getElementById("hud-root");
  private startEl = document.getElementById("start-overlay");
  private usernameInput = document.getElementById("start-username") as HTMLInputElement | null;
  private startOperatorNameEl = document.getElementById("start-operator-name");
  private scoreYouLabelEl = document.querySelector<HTMLElement>(".sb-you .sb-label");
  private startDiffSlider = document.getElementById("start-diff-slider") as HTMLInputElement | null;
  private startDiffValueEl = document.getElementById("start-diff-value");
  private startDiffNameEl = document.getElementById("start-diff-name");
  private startPreview: StartCharacterPreview | null = new StartCharacterPreview("start-character-preview");

  private pauseEl = document.getElementById("menu-overlay");
  private pauseScoreEl = document.getElementById("pause-score");
  private endEl = document.getElementById("end-overlay");
  private endPanelEl = document.getElementById("end-panel");
  private endEmblemEl = document.getElementById("end-emblem");
  private endTitleEl = document.getElementById("end-title");
  private endSubtitleEl = document.getElementById("end-subtitle");
  private endScoreEl = document.getElementById("end-score");
  private endVerdictEl = document.getElementById("end-verdict");
  private endStatsEl = document.getElementById("end-stats");
  private countUpRaf: number | null = null;
  private diffSlider = document.getElementById("diff-slider") as HTMLInputElement | null;
  private diffValueEl = document.getElementById("diff-value");
  private diffNameEl = document.getElementById("diff-name");

  // "Mute Trash Talk" toggles — one on the start menu, one in the pause menu;
  // both reflect (and write) the same persisted setting.
  private trashToggleEls = [document.getElementById("btn-trash-start"), document.getElementById("btn-trash-pause")];
  private trashTalkMuted = Settings.getTrashTalkMuted();

  // Graphics quality pickers — start screen and pause menu, same setting
  private qualityPickers = Array.from(document.querySelectorAll<HTMLElement>("[data-quality-picker]"));
  private qualityDescs = Array.from(document.querySelectorAll<HTMLElement>("[data-quality-desc]"));

  // Opponent pickers — start screen and end screen, one saved choice
  private rivalPickers = Array.from(document.querySelectorAll<HTMLElement>("[data-rival-picker]"));
  private rivalSampleBtns = Array.from(document.querySelectorAll<HTMLElement>("[data-rival-sample]"));
  private toastEl = document.getElementById("hud-toast");
  private toastTimer: ReturnType<typeof setTimeout> | null = null;
  private feedTimers = new Set<ReturnType<typeof setTimeout>>();
  private listeners = new AbortController();
  private unsubscribe: Array<() => void> = [];

  private lastYou = -1;
  private lastEnemy = -1;
  private lastEnemy2 = -1;

  // Match stats for the end screen
  private streak = 0;
  private bestStreak = 0;
  private headshots = 0;
  private totalScore = 0;

  private effects: Effects;
  private getPlayerWeaponId: () => string;

  constructor(effects: Effects, getPlayerWeaponId: () => string, callbacks: MatchUICallbacks) {
    this.effects = effects;
    this.getPlayerWeaponId = getPlayerWeaponId;
    const options = { signal: this.listeners.signal };
    document.getElementById("btn-start")?.addEventListener("click", () => this.startFromMenu(callbacks.onStart), options);
    document.getElementById("btn-resume")?.addEventListener("click", () => callbacks.onResume(), options);
    document.getElementById("btn-end")?.addEventListener("click", () => callbacks.onEndMatch(), options);
    document.getElementById("btn-again")?.addEventListener("click", () => callbacks.onPlayAgain(), options);

    this.usernameInput?.addEventListener("input", () => this.renderUsernamePreview(), options);
    this.usernameInput?.addEventListener(
      "keydown",
      (event) => {
        if (event.key !== "Enter") return;
        event.preventDefault();
        this.startFromMenu(callbacks.onStart);
      },
      options
    );
    this.renderUsernamePreview();

    for (const slider of [this.startDiffSlider, this.diffSlider]) {
      slider?.addEventListener(
        "input",
        () => {
          const level = parseInt(slider.value, 10);
          this.renderDifficulty(level);
          callbacks.onDifficultyChange(level);
        },
        options
      );
    }

    for (const el of this.trashToggleEls) {
      el?.addEventListener(
        "click",
        () => {
          this.trashTalkMuted = !this.trashTalkMuted;
          Settings.setTrashTalkMuted(this.trashTalkMuted);
          this.renderTrashTalk();
          callbacks.onToggleTrashTalk(this.trashTalkMuted);
        },
        options
      );
    }
    this.renderTrashTalk();

    for (const picker of this.qualityPickers) {
      picker.addEventListener(
        "click",
        (event) => {
          const btn = (event.target as HTMLElement).closest<HTMLElement>("[data-quality]");
          const quality = btn?.dataset.quality as GraphicsQuality | undefined;
          if (!quality) return;
          this.renderGraphics(quality);
          callbacks.onGraphicsChange(quality);
        },
        options
      );
    }
    this.renderGraphics(Settings.getGraphicsQuality());

    for (const picker of this.rivalPickers) {
      picker.addEventListener(
        "click",
        (event) => {
          const btn = (event.target as HTMLElement).closest<HTMLElement>("[data-rival]");
          const choice = btn?.dataset.rival as OpponentChoice | undefined;
          if (!choice) return;
          Settings.setOpponentChoice(choice);
          this.renderRival(choice);
        },
        options
      );
    }
    for (const btn of this.rivalSampleBtns) {
      btn.addEventListener("click", () => callbacks.onPreviewRival(Settings.getOpponentChoice()), options);
    }
    this.renderRival(Settings.getOpponentChoice());

    this.unsubscribe.push(
      MatchEvents.on("kill", (e) => this.onKill(e.headshot, e.cause, e.victim)),
      MatchEvents.on("playerDeath", (e) => this.onPlayerDeath(e.weaponId, e.self ?? false, e.killer)),
      MatchEvents.on("rivalKill", (e) =>
        this.addFeedRow(rivalById(e.killer).name, WEAPON_NAMES[e.weaponId] ?? "—", rivalById(e.victim).name, "rivals")
      )
    );
  }

  // ---------------------------------------------------------------- graphics

  private renderRival(choice: OpponentChoice): void {
    for (const picker of this.rivalPickers) {
      for (const btn of picker.querySelectorAll<HTMLElement>("[data-rival]")) {
        btn.setAttribute("aria-checked", btn.dataset.rival === choice ? "true" : "false");
      }
    }
  }

  public renderGraphics(quality: GraphicsQuality): void {
    this.startPreview?.setQuality(quality);
    for (const picker of this.qualityPickers) {
      for (const btn of picker.querySelectorAll<HTMLElement>("[data-quality]")) {
        btn.setAttribute("aria-checked", btn.dataset.quality === quality ? "true" : "false");
      }
    }
    for (const el of this.qualityDescs) el.textContent = QUALITY_DESC[quality];
  }

  // A short notice over the HUD (the automatic quality step-down uses it)
  public toast(text: string, ms: number = 4000): void {
    if (!this.toastEl) return;
    this.toastEl.textContent = text;
    this.toastEl.classList.remove("hidden");
    this.toastEl.style.animation = "none";
    void this.toastEl.offsetWidth;
    this.toastEl.style.animation = "";
    if (this.toastTimer) clearTimeout(this.toastTimer);
    this.toastTimer = setTimeout(() => this.toastEl?.classList.add("hidden"), ms);
  }

  // ------------------------------------------------------------- scoreboard

  public setScore(you: number, enemies: readonly ScoreLine[]): void {
    const enemy = enemies[0]?.score ?? 0;
    const enemy2 = enemies[1]?.score ?? -1; // free-for-all only
    if (you === this.lastYou && enemy === this.lastEnemy && enemy2 === this.lastEnemy2) return;
    const youChanged = you !== this.lastYou && this.lastYou >= 0;
    const enemyChanged = enemy !== this.lastEnemy && this.lastEnemy >= 0;
    const enemy2Changed = enemy2 !== this.lastEnemy2 && this.lastEnemy2 >= 0;
    this.lastYou = you;
    this.lastEnemy = enemy;
    this.lastEnemy2 = enemy2;
    if (this.youEl) this.youEl.textContent = you.toString();
    if (this.enemyEl) this.enemyEl.textContent = enemy.toString();
    if (this.enemy2El && enemy2 >= 0) this.enemy2El.textContent = enemy2.toString();
    if (youChanged && this.youEl?.parentElement) this.pulse(this.youEl.parentElement, "sb-pulse");
    if (enemyChanged && this.enemyEl?.parentElement) this.pulse(this.enemyEl.parentElement, "sb-pulse");
    if (enemy2Changed && this.enemy2El?.parentElement) this.pulse(this.enemy2El.parentElement, "sb-pulse");
    // the leading side glows
    this.scoreboardEl?.classList.toggle("you-lead", you > Math.max(enemy, enemy2));
    this.scoreboardEl?.classList.toggle("enemy-lead", enemy > you && enemy >= enemy2);
    this.scoreboardEl?.classList.toggle("enemy2-lead", enemy2 > you && enemy2 > enemy);
  }

  // Duel: the other side is "OPFOR". Free-for-all: each rival by name.
  private enemyName(id: RivalId | undefined): string {
    return isFreeForAll() && id ? rivalById(id).name : "OPFOR";
  }

  // ------------------------------------------------------------ kill events

  private onKill(headshot: boolean, cause: "player" | "airstrike" | "apache", victim: RivalId): void {
    this.streak++;
    this.bestStreak = Math.max(this.bestStreak, this.streak);
    if (headshot) this.headshots++;
    const points = headshot ? 150 : 100;
    this.totalScore += points;

    // streak kills credit the streak in the feed, not whatever gun is in hand
    const weapon = cause === "player" ? (WEAPON_NAMES[this.getPlayerWeaponId()] ?? "—") : WEAPON_NAMES[cause];
    this.banner(headshot ? "HEADSHOT" : "ENEMY DOWN", headshot);
    this.floatPoints(`+${points}`, headshot);
    this.addFeedRow(getUserDisplayName(), weapon, this.enemyName(victim), "");
    this.effects.playKillSound(headshot);

    for (const [count, label, tier] of STREAK_CALLS) {
      if (this.streak === count) {
        this.streakCall(`${label} · ${count}`);
        this.effects.playStreakSound(tier);
      }
    }
  }

  private onPlayerDeath(weaponId: string, self: boolean, killer: RivalId | undefined): void {
    this.streak = 0;
    const left = self ? getUserDisplayName() : this.enemyName(killer);
    this.addFeedRow(left, WEAPON_NAMES[weaponId] ?? "—", getUserDisplayName(), "enemy");
  }

  // --------------------------------------------------------------- juice bits

  // Restart a CSS animation on an element that may still be mid-animation
  private pulse(el: HTMLElement, klass: string): void {
    el.classList.remove(klass);
    void el.offsetWidth; // reflow resets the animation clock
    el.classList.add(klass);
  }

  private banner(title: string, headshot: boolean): void {
    if (!this.bannerEl || !this.bannerTitleEl || !this.bannerSubEl) return;
    this.bannerTitleEl.textContent = title;
    this.bannerSubEl.textContent = this.streak > 1 ? `KILLSTREAK ×${this.streak}` : "";
    this.bannerEl.classList.toggle("headshot", headshot);
    this.bannerEl.classList.remove("hidden");
    this.pulse(this.bannerEl, "play");
  }

  private streakCall(text: string): void {
    if (!this.streakEl) return;
    this.streakEl.textContent = text;
    this.streakEl.classList.remove("hidden");
    this.pulse(this.streakEl, "play");
  }

  private floatPoints(text: string, headshot: boolean): void {
    if (!this.pointsEl || typeof document === "undefined" || typeof document.createElement !== "function") return;
    const span = document.createElement("span");
    span.className = headshot ? "pt headshot" : "pt";
    span.textContent = text;
    span.style.setProperty("--drift", `${(Math.random() * 56 - 28).toFixed(0)}px`);
    this.pointsEl.appendChild(span);
    span.addEventListener("animationend", () => span.remove());
  }

  // kind: "" the player scored, "enemy" the player died, "rivals" one rival dropped the other
  private addFeedRow(left: string, weapon: string, right: string, kind: "" | "enemy" | "rivals"): void {
    if (!this.feedEl || typeof document === "undefined" || typeof document.createElement !== "function") return;
    const row = document.createElement("div");
    row.className = kind ? `kf-row ${kind}` : "kf-row";
    const leftEl = document.createElement("b");
    const weaponEl = document.createElement("span");
    const rightEl = document.createElement("b");
    leftEl.textContent = left;
    weaponEl.className = "kf-weapon";
    weaponEl.textContent = `[ ${weapon} ]`;
    rightEl.textContent = right;
    row.append(leftEl, weaponEl, rightEl);
    this.feedEl.prepend(row);
    while (this.feedEl.children.length > 5) this.feedEl.lastElementChild?.remove();
    const timer = setTimeout(() => {
      this.feedTimers.delete(timer);
      row.remove();
    }, 5000);
    this.feedTimers.add(timer);
  }

  // ----------------------------------------------------------- pause + end

  public showStart(level: number): void {
    this.renderDifficulty(level);
    this.renderUsernamePreview();
    this.hudRootEl?.classList.add("start-screen-active");
    this.startEl?.classList.remove("hidden");
    if (!this.startPreview) {
      this.startPreview = new StartCharacterPreview("start-character-preview");
      this.startPreview.setQuality(Settings.getGraphicsQuality());
    }
    this.startPreview.start();
  }

  public hideStart(): void {
    this.hudRootEl?.classList.remove("start-screen-active");
    this.startEl?.classList.add("hidden");
    this.startPreview?.dispose();
    this.startPreview = null;
  }

  public showPause(you: number, enemies: readonly ScoreLine[], level: number): void {
    if (this.pauseScoreEl) {
      this.pauseScoreEl.textContent =
        enemies.length > 1
          ? [`YOU ${you}`, ...enemies.map((e) => `${e.name} ${e.score}`)].join(" · ")
          : `YOU ${you} — ${enemies[0]?.score ?? 0} OPFOR`;
    }
    this.renderDifficulty(level);
    this.pauseEl?.classList.remove("hidden");
  }

  public hidePause(): void {
    this.pauseEl?.classList.add("hidden");
  }

  // deaths: the player's deaths (in a free-for-all, rival scores also count kills of each other)
  public showEnd(result: MatchResult, you: number, enemies: readonly ScoreLine[], deaths: number): void {
    this.hidePause();
    this.cancelCountUps();

    const meta = RESULT_META[result];
    const youWon = result === "victory";
    const enemyWon = result === "defeat";
    // the rival who won (free-for-all: the top scorer)
    let leader: ScoreLine | undefined = enemies[0];
    for (const e of enemies) if (leader && e.score > leader.score) leader = e;

    if (this.endPanelEl) this.endPanelEl.className = `end-panel result-${result}`;
    if (this.endEmblemEl) this.endEmblemEl.innerHTML = meta.emblem;

    if (this.endTitleEl) {
      this.endTitleEl.textContent = meta.title;
      this.endTitleEl.className = result;
    }

    if (this.endSubtitleEl) this.endSubtitleEl.textContent = meta.subtitle;

    if (this.endScoreEl && typeof document !== "undefined" && typeof document.createElement === "function") {
      const makeRow = (
        cls: string,
        leftText: string,
        rightText: string,
        isWinner = false,
        count?: number,
        badge = false
      ): HTMLDivElement => {
        const row = document.createElement("div");
        row.className = isWinner ? `end-score-row ${cls} is-winner` : `end-score-row ${cls}`;
        const left = document.createElement("span");
        const right = document.createElement("span");
        if (count !== undefined) {
          left.className = "end-username";
          left.textContent = leftText;
          if (badge) {
            const win = document.createElement("span");
            win.className = "end-winner";
            win.textContent = "WINNER";
            left.appendChild(win);
          }
          right.className = "end-score-num";
          right.dataset.count = count.toString();
        } else {
          left.textContent = leftText;
        }
        right.textContent = rightText;
        row.append(left, right);
        return row;
      };
      this.endScoreEl.replaceChildren(
        makeRow("end-score-head", "OPERATOR", "SCORE"),
        makeRow("you", getUserDisplayName(), "0", youWon, you, youWon),
        ...enemies.map((e) => {
          const won = enemyWon && e === leader;
          return makeRow("enemy", e.name, "0", won, e.score, won);
        })
      );
    }

    if (this.endVerdictEl) {
      this.endVerdictEl.textContent = meta.verdict(enemyWon && leader ? leader.name : enemies.map((e) => e.name).join(" and "));
    }

    const kd = deaths === 0 ? you.toFixed(2) : (you / deaths).toFixed(2);
    if (this.endStatsEl && typeof document !== "undefined" && typeof document.createElement === "function") {
      const stats: ReadonlyArray<[string, number | string, number]> = [
        ["KILLS", you, 0],
        ["DEATHS", deaths, 0],
        ["K / D", kd, 2],
        ["HEADSHOTS", this.headshots, 0],
        ["BEST STREAK", this.bestStreak, 0],
        ["SCORE", this.totalScore, 0],
      ];
      this.endStatsEl.replaceChildren(
        ...stats.map(([label, value, decimals]) => {
          const stat = document.createElement("div");
          stat.className = "stat";
          const valEl = document.createElement("span");
          valEl.className = "stat-v";
          valEl.dataset.count = String(value);
          valEl.dataset.decimals = decimals.toString();
          valEl.textContent = decimals ? (0).toFixed(decimals) : "0";
          const keyEl = document.createElement("span");
          keyEl.className = "stat-k";
          keyEl.textContent = label;
          stat.append(valEl, keyEl);
          return stat;
        })
      );
    }

    this.endEl?.classList.remove("hidden");
    // Roll every numeric readout up from zero once the panel is on screen
    this.startCountUps();

    if (result === "victory") this.effects.playVictorySound();
    else this.effects.playDefeatSound();
  }

  // Animate all [data-count] readouts inside the end screen from 0 to target in one rAF loop.
  private startCountUps(): void {
    const els = this.endEl?.querySelectorAll<HTMLElement>("[data-count]");
    if (!els || els.length === 0) return;
    const DURATION = 850;
    const base = performance.now();
    const items: Array<{ el: HTMLElement; target: number; decimals: number; start: number }> = [];
    els.forEach((el, i) => {
      const target = parseFloat(el.dataset.count ?? "0");
      const decimals = parseInt(el.dataset.decimals ?? "0", 10);
      if (isFinite(target)) items.push({ el, target, decimals, start: base + i * 45 });
    });
    if (items.length === 0) return;

    const tick = (now: number): void => {
      let pending = false;
      for (const item of items) {
        const t = Math.min(1, Math.max(0, (now - item.start) / DURATION));
        // easeOutCubic for a quick-then-settle count
        const eased = 1 - Math.pow(1 - t, 3);
        const val = item.target * eased;
        item.el.textContent = item.decimals ? val.toFixed(item.decimals) : Math.round(val).toString();
        if (t < 1) pending = true;
      }
      this.countUpRaf = pending ? requestAnimationFrame(tick) : null;
    };
    this.countUpRaf = requestAnimationFrame(tick);
  }

  private cancelCountUps(): void {
    if (this.countUpRaf !== null) {
      cancelAnimationFrame(this.countUpRaf);
      this.countUpRaf = null;
    }
  }

  public resetMatch(): void {
    this.clearTimers();
    this.streak = 0;
    this.bestStreak = 0;
    this.headshots = 0;
    this.totalScore = 0;
    this.lastYou = -1;
    this.lastEnemy = -1;
    this.lastEnemy2 = -1;
    // Duel: "OPFOR" on the right. Free-for-all: both rivals by name.
    const rivals = getRivals();
    const ffa = rivals.length > 1;
    this.scoreboardEl?.classList.toggle("ffa", ffa);
    if (this.enemyLabelEl) this.enemyLabelEl.textContent = ffa ? rivals[0].name : "OPFOR";
    if (this.enemy2LabelEl) this.enemy2LabelEl.textContent = ffa ? rivals[1].name : "";
    if (this.enemy2El) this.enemy2El.textContent = "0";
    this.setScore(0, []);
    if (this.feedEl) this.feedEl.replaceChildren();
    if (this.pointsEl) this.pointsEl.replaceChildren();
    this.toastEl?.classList.add("hidden");
    this.bannerEl?.classList.add("hidden");
    this.streakEl?.classList.add("hidden");
    this.cancelCountUps();
    this.endEl?.classList.add("hidden");
    this.hideStart();
    this.hidePause();
  }

  public dispose(): void {
    this.listeners.abort();
    for (const off of this.unsubscribe) off();
    this.unsubscribe.length = 0;
    this.clearTimers();
    this.cancelCountUps();
    this.startPreview?.dispose();
    this.startPreview = null;
  }

  private clearTimers(): void {
    if (this.toastTimer !== null) clearTimeout(this.toastTimer);
    this.toastTimer = null;
    for (const timer of this.feedTimers) clearTimeout(timer);
    this.feedTimers.clear();
  }

  private renderTrashTalk(): void {
    for (const el of this.trashToggleEls) {
      if (!el) continue;
      el.setAttribute("aria-pressed", this.trashTalkMuted ? "true" : "false");
      const stateEl = el.querySelector(".trash-toggle-state");
      if (stateEl) stateEl.textContent = this.trashTalkMuted ? "ON" : "OFF";
    }
  }

  private startFromMenu(onStart: () => void): void {
    const displayName = setUserDisplayName(this.usernameInput?.value ?? "");
    if (this.usernameInput) this.usernameInput.value = displayName;
    this.renderUsernamePreview();
    onStart();
  }

  private renderUsernamePreview(): void {
    const displayName = normalizeUserDisplayName(this.usernameInput?.value ?? getUserDisplayName());
    if (this.startOperatorNameEl) this.startOperatorNameEl.textContent = displayName;
    if (this.scoreYouLabelEl) this.scoreYouLabelEl.textContent = displayName;
  }

  private renderDifficulty(level: number): void {
    const name = difficultyLevelName(level);
    const levelText = level.toString();
    for (const slider of [this.diffSlider, this.startDiffSlider]) {
      if (slider) slider.value = levelText;
    }
    for (const el of [this.diffValueEl, this.startDiffValueEl]) {
      if (el) el.textContent = levelText;
    }
    for (const el of [this.diffNameEl, this.startDiffNameEl]) {
      if (!el) continue;
      el.textContent = name;
      // the top tiers get the liquid-metal sheen in the menu too
      el.classList.toggle("terminator", level >= TERMINATOR_LEVEL);
    }
  }
}
