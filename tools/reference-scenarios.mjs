const Fire = 1,
  Aim = 2,
  Sprint = 4,
  Jump = 8,
  Stance = 16,
  Reload = 32,
  Swap = 64,
  Streak = 128;
const defaultStart = {
  seed: 1179599909,
  difficulty: 3,
  flags: 0,
  player: [-13.5, 0, -13.5],
  yaw: Math.PI / 4,
  pitch: 0,
  rival: [13.5, 0, 13.5],
  rivalYaw: (-3 * Math.PI) / 4,
  weapon: 0,
  clip: -1,
  score: [0, 0],
  streak: 0,
  deathTick: 0,
};

function scenario(name, length, start, cue) {
  let old = 0;
  const inputs = Array.from({ length }, (_, i) => {
    const [x = 0, y = 0, lookX = 0, lookY = 0, held = 0, tap = 0] = cue(i + 1);
    const input = [x, y, lookX, lookY, held, (held & ~old) | tap];
    old = held;
    return input;
  });
  const checkpoints = {
    "long-lane": [65, 163],
    apache: [120, 195],
    "weapon-cycle": [80, 262, 346, 578],
    laptop: [49],
    "death-time": [80],
    glass: [35, 146],
  };
  return {
    name,
    start: { ...defaultStart, ...start },
    inputs,
    pictures: checkpoints[name] ?? [],
    tolerance: { position: 0.005, angle: 0.00005, time: 0.00000005 },
  };
}

export const scenarios = [
  scenario("long-lane", 360, { player: [-14.3, 0, -12.5], rival: [-14.3, 0, 12.8], yaw: 0, rivalYaw: Math.PI }, (t) => {
    if (t < 32) return [0, 1, 0, 0, Sprint];
    if (t < 134) return [0, +!(t % 3), t < 60 ? 0.0009 : -0.0003, 0, Aim | (t >= 72 && t < 105 ? Fire : 0)];
    if (t === 135) return [0, 0, 0, 0, 0, Reload];
    if (t < 251) return [0, 0, t > 180 && t < 209 ? -0.0005 : 0, 0, 0];
    if (t < 340) return [0, 0, 0, 0, Aim | (t > 299 ? Fire : 0)];
    return [];
  }),
  scenario("apache", 570, { flags: 4, yaw: Math.PI / 4, rival: [-12.5, 0, 12.5], rivalYaw: Math.PI }, (t) => {
    if (t === 1) return [0, 0, 0, 0, 0, Streak];
    if (t < 26) return [0, 0, 0, -0.016];
    if (t < 160) return [];
    if (t < 235) return [0, 0, -0.0035, 0];
    if (t < 310) return [0, 0, -0.006, 0.00512];
    if (t < 410) return [0, 0, 0, 0, Aim | (t >= 363 && t < 390 ? Fire : 0)];
    return [];
  }),
  scenario("weapon-cycle", 760, { flags: 1, yaw: 2.8 }, (t) => {
    if (t < 25) return [0, 0, 0, 0, Fire];
    if (t === 27 || t === 315 || t === 542) return [0, 0, 0, 0, 0, Reload];
    if (t === 182 || t === 465) return [0, 0, 0, 0, 0, Swap];
    if (t > 230 && t < 281) return [0, 0, 0, 0, Aim | (t >= 267 ? Fire : 0)];
    if (t > 505 && t < 534) return [0, 0, 0, 0, Aim | (t >= 524 ? Fire : 0)];
    return [];
  }),
  scenario("movement", 340, { flags: 1 }, (t) => {
    if (t < 25) return [1, 1];
    if (t < 66) return [0, 1, 0.006, -0.001, Sprint];
    if (t < 101) return [0, 1, 0, 0, Jump];
    if (t < 106) return [0, 0, 0, 0, Stance];
    if (t < 140) return [-1, 0];
    if (t < 172) return [0, 0, 0, 0, Stance];
    if (t < 205) return [0, -1];
    if (t === 208) return [0, 0, 0, 0, 0, Stance];
    if (t < 266) return [0, -1, -0.013, 0.001];
    if (t < 310) return [1, 0, 0, 0, Aim];
    return [];
  }),
  scenario("laptop", 390, { flags: 3 }, (t) => {
    if (t === 1) return [0, 0, 0, 0, Fire, Streak];
    if (t < 55 && t > 30) return [0, 0, 0.011, -0.008];
    if (t >= 55 && t < 79) return [0, 0, 0, 0, Fire];
    return [];
  }),
  scenario("death-time", 350, { flags: 1, deathTick: 21 }, (t) => {
    if (t < 20) return [0, 0, 0, 0, Fire];
    if (t > 29 && t < 285) return [0, 0, 0, 0, Fire];
    return [];
  }),
  scenario("glass", 185, { flags: 1, weapon: 1 }, (t) => [0, 0, 0, 0, Aim | (t === 36 || t === 145 ? Fire : 0)]),
  scenario(
    "match-point",
    121,
    { flags: 1, player: [-14.3, 0, -12.5], rival: [-14.3, 0, -7.5], yaw: 0, pitch: 0.068, weapon: 1, score: [9, 0] },
    (t) => [0, 0, 0, 0, Aim | (t === 121 ? Fire : 0)]
  ),
];
