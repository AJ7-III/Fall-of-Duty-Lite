export const DEFAULT_USER_DISPLAY_NAME = "Player 1";
// The rivals: their name is on comms, the scoreboard and the end screen, and
// their voice lines play over kills and deaths. The id is their folder under
// src/assets/voice. A match is a duel against one of them, or a three-way
// free-for-all where both are in the yard and fight each other as well.
export type RivalId = "ipwndurmom" | "hanz";
export interface Rival {
  readonly id: RivalId;
  readonly name: string;
  readonly edge: number; // how much harder their squad plays than the difficulty slider (0.1 = 10%)
}
export const RIVALS: readonly Rival[] = [
  { id: "ipwndurmom", name: "IPwndUrMom", edge: 0 },
  { id: "hanz", name: "Hanz", edge: 0.1 },
];
export type OpponentChoice = RivalId | "ffa";

let opponents: readonly Rival[] = [RIVALS[0]];

// This match's rivals: one in a duel, all of them in a free-for-all
export function getRivals(): readonly Rival[] {
  return opponents;
}

export function isFreeForAll(): boolean {
  return opponents.length > 1;
}

export function rivalById(id: RivalId): Rival {
  return RIVALS.find((r) => r.id === id) ?? RIVALS[0];
}

// Lock in the opponent picker's choice for the next match
export function setOpponents(choice: OpponentChoice): readonly Rival[] {
  opponents = choice === "ffa" ? RIVALS : [rivalById(choice)];
  return opponents;
}

let userDisplayName = DEFAULT_USER_DISPLAY_NAME;

export function normalizeUserDisplayName(value: string): string {
  const normalized = value.trim().replace(/\s+/g, " ").slice(0, 24);
  return normalized || DEFAULT_USER_DISPLAY_NAME;
}

export function setUserDisplayName(value: string): string {
  userDisplayName = normalizeUserDisplayName(value);
  return userDisplayName;
}

export function getUserDisplayName(): string {
  return userDisplayName;
}
