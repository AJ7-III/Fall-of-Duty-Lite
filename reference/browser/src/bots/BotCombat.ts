import type { Vector3 } from "@babylonjs/core";
import { rangeCurve } from "./BotConfig";
import type { BotWeaponProfile } from "./BotConfig";

// Everything a bot reads from whoever it is fighting. The player satisfies it
// as-is; in a free-for-all BotManager wraps the other rival's soldier in one.
export interface Combatant {
  readonly position: Vector3;
  readonly velocity: Vector3;
  readonly eyeHeight: number;
  readonly isDead: boolean;
  readonly isCrouching: boolean;
  readonly isProne: boolean;
  readonly isSprinting: boolean;
  // attacker: the bot that fired, so a soldier killed by the other rival is credited to them
  takeDamage(amount: number, fromPos: Vector3, attacker?: unknown): boolean;
}

// Next cooldown after a shot. The overshoot carries into the new interval:
// resetting to the full interval rounds every shot up to a whole frame, which
// made a bot's rate of fire (and so its damage per second) depend on the
// player's FPS. The clamp stops a long idle from banking a burst of free shots.
export function nextFireTimer(timer: number, interval: number, dt: number): number {
  return Math.max(timer, -dt) + interval;
}

export function rayVsVerticalCylinder(
  origin: Vector3,
  dir: Vector3,
  cx: number,
  baseY: number,
  cz: number,
  radius: number,
  height: number
): number {
  const rx = origin.x - cx;
  const rz = origin.z - cz;
  const a = dir.x * dir.x + dir.z * dir.z;
  if (a < 1e-8) return Infinity;
  const b = 2 * (rx * dir.x + rz * dir.z);
  const c = rx * rx + rz * rz - radius * radius;
  const disc = b * b - 4 * a * c;
  if (disc < 0) return Infinity;
  let t = (-b - Math.sqrt(disc)) / (2 * a);
  if (t < 0) {
    // Muzzle already inside the cylinder (point-blank): take the exit
    // root so contact shots still land instead of passing clean through
    t = (-b + Math.sqrt(disc)) / (2 * a);
    if (t < 0) return Infinity;
  }
  const y = origin.y + dir.y * t;
  if (y < baseY || y > baseY + height) return Infinity;
  return t;
}

export function rayVsPlayerBody(origin: Vector3, dir: Vector3, p: Combatant): number {
  return rayVsVerticalCylinder(origin, dir, p.position.x, p.position.y, p.position.z, 0.42, p.eyeHeight + 0.15);
}

export function angleDelta(from: number, to: number): number {
  let d = (to - from) % (Math.PI * 2);
  if (d > Math.PI) d -= Math.PI * 2;
  if (d < -Math.PI) d += Math.PI * 2;
  return d;
}

export function selectWeapon(
  weapons: ReadonlyArray<{ clip: number; profile: BotWeaponProfile }>,
  active: number,
  distance: number,
  skill: number
): number {
  const primaryDry = weapons[0].clip === 0 ? 1.8 : 1;
  const scores = weapons.map((weapon, i) => {
    const ammo = 0.3 + 0.7 * (weapon.clip / weapon.profile.magSize);
    return rangeCurve(distance, weapon.profile.range) * ammo * (i === 1 ? primaryDry * 0.75 : 1);
  });
  const best = scores[0] >= scores[1] ? 0 : 1;
  return best !== active && scores[best] > scores[active] * (1 + (1 - skill) * 0.9) ? best : active;
}
