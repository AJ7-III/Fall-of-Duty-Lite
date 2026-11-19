// Persisted client-side game settings (localStorage-backed). Kept tiny and
// dependency-free so both the UI layer (which toggles a setting) and the
// systems that consume it (e.g. RivalVoice reading its initial mute state)
// can share one source of truth without a circular import.

import type { OpponentChoice } from "./MatchNames";

const TRASH_TALK_KEY = "fallOfDuty.muteTrashTalk";
const OPPONENT_KEY = "fallOfDuty.opponent";

const OPPONENT_CHOICES: ReadonlyArray<OpponentChoice> = ["ipwndurmom", "hanz", "ffa"];
const GRAPHICS_KEY = "fallOfDuty.graphics";
// The tier the frame-rate guard last stepped down to. Kept apart from the
// player's own pick so it can never override a deliberate choice.
const AUTO_GRAPHICS_KEY = "fallOfDuty.graphicsAuto";

export type GraphicsQuality = "high" | "balanced" | "performance";
const GRAPHICS_LEVELS: ReadonlyArray<GraphicsQuality> = ["high", "balanced", "performance"];

function read(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null; // storage disabled (private mode / blocked) — use defaults
  }
}

function write(key: string, value: string): void {
  try {
    localStorage.setItem(key, value);
  } catch {
    // storage disabled — the setting simply won't persist across reloads
  }
}

export const Settings = {
  getTrashTalkMuted(): boolean {
    const v = read(TRASH_TALK_KEY);
    return v === null ? true : v === "1";
  },
  setTrashTalkMuted(muted: boolean): void {
    write(TRASH_TALK_KEY, muted ? "1" : "0");
  },
  // The player's pick wins, then the tier the frame-rate guard settled on in
  // an earlier session (so a slow machine doesn't repeat the slow start
  // every reload), then the default
  getGraphicsQuality(): GraphicsQuality {
    for (const key of [GRAPHICS_KEY, AUTO_GRAPHICS_KEY]) {
      const v = read(key) as GraphicsQuality | null;
      if (v && GRAPHICS_LEVELS.includes(v)) return v;
    }
    return "high";
  },
  setGraphicsQuality(q: GraphicsQuality): void {
    write(GRAPHICS_KEY, q);
  },
  setAutoGraphicsQuality(q: GraphicsQuality): void {
    write(AUTO_GRAPHICS_KEY, q);
  },
  getOpponentChoice(): OpponentChoice {
    const v = read(OPPONENT_KEY) as OpponentChoice | null;
    return v && OPPONENT_CHOICES.includes(v) ? v : "ipwndurmom";
  },
  setOpponentChoice(choice: OpponentChoice): void {
    write(OPPONENT_KEY, choice);
  },
};
