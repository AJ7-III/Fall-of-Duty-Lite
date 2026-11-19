import { chromium } from "@playwright/test";
import { createServer } from "vite";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { access, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { parseArgs } from "node:util";
import { tmpdir } from "node:os";
import { scenarios } from "./reference-scenarios.mjs";

const root = fileURLToPath(new URL("../", import.meta.url));
const browserRoot = resolve(root, "reference/browser");
const publicDir = resolve(browserRoot, "public");
const { values } = parseArgs({
  options: {
    out: { type: "string", default: resolve(root, "out/reference") },
    pack: { type: "string", default: resolve(root, "assets/fall-of-duty.fodp.json") },
    scene: { type: "string", multiple: true },
  },
});
try {
  await access(resolve(publicDir, "models/soldier.glb.gz"));
} catch {
  throw new Error("Browser capture requires the original reference soldier model. See ASSET_PIPELINE.md.");
}
const manifest = JSON.parse(await readFile(values.pack, "utf8"));
const output = resolve(values.out);
await mkdir(output, { recursive: true });
const selected = scenarios.filter((item) => !values.scene || values.scene.includes(item.name));
if (!selected.length || (values.scene && selected.length !== values.scene.length)) throw new Error("Unknown reference scene.");

const locations = {};
const patterns = {
  "src/weapons/WeaponTypes.ts": [
    ["right.scaleAndAddToRef((Math.random()", "shot.spread.x"],
    ["up.scaleAndAddToRef((Math.random()", "shot.spread.y"],
  ],
  "src/player/CameraRig.ts": [["this.recoilYaw += ((Math.random()", "camera.recoil.yaw"]],
  "src/anim/DeathPerformance.ts": [["this.variant = variant ??", "death.variant"]],
  "src/bots/BotManager.ts": [
    ["+ Math.random() * 2;", "player.spawn.corner"],
    ["+ Math.random() * 11;", "bot.spawn.corner"],
    ["Math.random() < 0.45 + skill", "bot.spawn.remember"],
  ],
  "src/bots/Bot.ts": [
    ["+ rand(0.15, 0.4)", "bot.suspicion.initial"],
    ["+ rand(0.1, 0.35)", "bot.patrol.initial"],
    ["+ rand(2.5, 4)", "bot.intel.nextSuspicion"],
    ["player.position.x + rand(-errMeters", "bot.spawnIntuition.x"],
    ["player.position.z + rand(-errMeters", "bot.spawnIntuition.z"],
    ["- rand(1.5, 7)", "bot.spawnIntuition.age"],
    ["* rand(0.8, 1.3)", "bot.reaction"],
    ["p.position.x + rand(-err", "bot.hearing.x"],
    ["p.position.z + rand(-err", "bot.hearing.z"],
    ["+ rand(3.5, 6.5)", "bot.retreat.duration"],
    ["+ rand(d.holdTime", "bot.combat.hold"],
    ["Math.random() < d.flankChance", "bot.combat.flank"],
    ["+ rand(1.0, 1.8)", "bot.combat.route"],
    ["+ rand(1.5, 2.8)", "bot.hunt.route"],
    ["+ rand(0.2, 0.6)", "bot.patrol.next"],
    ["p.position.x + rand(-uncertainty", "bot.suspicion.x"],
    ["p.position.z + rand(-uncertainty", "bot.suspicion.z"],
    ["+ rand(2.0, 3.8)", "bot.suspicion.next"],
    ["Math.random() < d.weaponSwitchSkill", "bot.weapon.quickSwap"],
    ["Math.random() < d.reloadDiscipline", "bot.weapon.topUp"],
    ["return score * rand(0.85, 1.15)", "bot.attack.score"],
    ["* rand(0.9, 1.1)", "bot.hide.score"],
    ["+ rand(1.2, 2.2)", "bot.hunt.recover"],
    ["this.errYaw = rand(-e", "bot.aimError.yaw"],
    ["this.errPitch = rand(-e", "bot.aimError.pitch"],
    ["Math.random() < d.aggression", "bot.burst.suppress"],
    ["Math.round(rand(lo, hi))", "bot.burst.length"],
    ["this.burstTimer = rand(lo, hi)", "bot.burst.cooldown"],
    ["this.aimYaw + rand(-spread", "bot.shot.yaw"],
    ["this.aimPitch + rand(-spread", "bot.shot.pitch"],
    ["this.errYaw += rand(-0.18", "bot.shot.recoil"],
  ],
  "src/bots/BotNav.ts": [["(Math.random() * this.count)", "bot.patrol.sample"]],
  "src/bots/BotNavigation.ts": [
    ["* rand(0.9, 1.1)", "bot.search.score"],
    ["* rand(0.85, 1.15)", "bot.patrol.score"],
  ],
  "src/killstreaks/Apache.ts": [
    ["this.orbitAngle = Math.random()", "apache.orbit"],
    ["now + 0.9 + Math.random()", "apache.burst.cooldown"],
    ["target.position.x - gunPos.x + (Math.random()", "apache.shot.x"],
    ["target.position.y + 1.1 - gunPos.y + (Math.random()", "apache.shot.y"],
    ["target.position.z - gunPos.z + (Math.random()", "apache.shot.z"],
  ],
  "src/killstreaks/Killstreaks.ts": [
    ["const angle = Math.random()", "airstrike.heading"],
    ["+ (Math.random() - 0.5) * 1.6,", "airstrike.bomb"],
  ],
};
for (const [file, rules] of Object.entries(patterns)) {
  const lines = (await readFile(resolve(browserRoot, file), "utf8")).split("\n");
  for (const [pattern, site] of rules) {
    const matches = lines.flatMap((line, i) => (line.includes(pattern) ? [i + 1] : []));
    if (matches.length !== 1 && site !== "airstrike.bomb") throw new Error(`Update the RNG map for ${file} ${pattern}`);
    for (const [i, line] of matches.entries()) {
      (locations[file] ??= {})[line - 1] = site === "airstrike.bomb" ? `${site}.${i === 0 ? "x" : "z"}` : site;
    }
  }
}

const instrument = {
  name: "fod-reference-gameplay-rng",
  enforce: "pre",
  transform(code, id) {
    const file = id.split("?")[0].slice(browserRoot.length + 1);
    if (!locations[file]) return;
    const lines = code.split("\n");
    for (const [line, site] of Object.entries(locations[file])) {
      const draw = `(globalThis.__fodGameplayDraw?.(${JSON.stringify(site)}) ?? Math.random())`;
      if (lines[line].includes("Math.random()")) lines[line] = lines[line].replace("Math.random()", draw);
      else if (lines[line].includes("rand(")) lines[line] = lines[line].replace("rand(", `((a, b) => a + (b - a) * ${draw})(`);
      else throw new Error(`Stale reference RNG rule ${site}`);
    }
    return lines.join("\n");
  },
};
const cacheDir = await mkdtemp(resolve(tmpdir(), "fod-reference-"));
const server = await createServer({
  configFile: false,
  root,
  publicDir,
  cacheDir,
  plugins: [instrument],
  server: { host: "127.0.0.1", port: 0, watch: null },
});
await server.listen();
let browser;
try {
  const { port } = server.httpServer.address();
  browser = await chromium.launch({
    headless: true,
    args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--mute-audio"],
  });
  async function run(scenario, expected, picture) {
    const page = await browser.newPage({ viewport: { width: 800, height: 450 }, deviceScaleFactor: 1 });
    page.on("pageerror", (error) => console.error(error.message));
    page.on("console", (message) => {
      if (message.type() === "error") console.error(message.text());
    });
    try {
      await page.addInitScript((seed) => {
        let n = seed;
        Math.random = () => {
          n ^= n << 13;
          n ^= n >>> 17;
          n ^= n << 5;
          return (n >>> 0) / 4294967296;
        };
        HTMLCanvasElement.prototype.requestPointerLock = () => Promise.resolve();
        localStorage.setItem("fallOfDuty.graphics", "performance");
      }, manifest.art.seed);
      await page.goto(`http://127.0.0.1:${port}/reference/browser/`);
      await page.waitForFunction(
        () => window.__game?.assetsReady || document.getElementById("startup-status")?.getAttribute("role") === "alert",
        null,
        { timeout: 180000 }
      );
      const startup = await page.evaluate(() => ({
        ready: !!window.__game?.assetsReady,
        message: document.getElementById("startup-message")?.textContent,
      }));
      if (!startup.ready) throw new Error(startup.message);
      const result = await page.evaluate(
        async (params) => {
          const { capture } = await import("/tools/browser-replay.mjs");
          return capture(params);
        },
        { scenario, expected }
      );
      if (picture) await page.screenshot({ path: picture, timeout: 60000 });
      return result;
    } finally {
      await page.close();
    }
  }
  for (const scene of selected) {
    const record = await run(scene, null, resolve(output, `${scene.name}.png`));
    const replay = await run({ ...scene, start: record.start }, record.draws, null);
    if (JSON.stringify(record.ticks) !== JSON.stringify(replay.ticks)) {
      const first = record.ticks.findIndex((tick, i) => JSON.stringify(tick) !== JSON.stringify(replay.ticks[i]));
      throw new Error(
        `${scene.name}: browser replay diverged at tick ${first + 1}\n${JSON.stringify(record.ticks[first])}\n${JSON.stringify(replay.ticks[first])}`
      );
    }
    const { pictures, ...reference } = record;
    for (const [tick, encoded] of pictures)
      await writeFile(resolve(output, `${scene.name}-t${tick}.png`), Buffer.from(encoded, "base64"));
    const s = record.start;
    const start = [
      "S",
      s.seed,
      s.difficulty,
      s.flags,
      ...s.player,
      s.yaw,
      s.pitch,
      ...s.rival,
      s.rivalYaw,
      s.weapon,
      s.clip,
      ...s.score,
      s.streak,
      s.deathTick,
    ].join(" ");
    const trace =
      [
        "FODTRACE 1",
        start,
        ...scene.inputs.map((row) => `T ${row.join(" ")}`),
        ...record.draws.map((row) => `R ${row.join(" ")}`),
      ].join("\n") + "\n";
    await writeFile(resolve(output, `${scene.name}.fodtrace`), trace);
    await writeFile(
      resolve(output, `${scene.name}.browser.json`),
      JSON.stringify({
        schema: 1,
        scene: scene.name,
        sourceRevision: manifest.sourceRevision,
        viewport: [800, 450],
        tickRate: 60,
        tolerance: scene.tolerance,
        browserReplayIdentical: true,
        ...reference,
      }) + "\n"
    );
    console.log(`${scene.name}: ${record.ticks.length} ticks, ${record.draws.length} gameplay draws, browser replay identical`);
  }
} finally {
  await browser?.close();
  await server.close();
  await rm(cacheDir, { recursive: true, force: true });
}
