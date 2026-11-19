import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import { Ray } from "@babylonjs/core/Culling/ray";
import { MatchEvents } from "../reference/browser/src/ui/MatchEvents.ts";

function generator(seed) {
  let state = seed || 1;
  return () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return (state >>> 0) / 4294967296;
  };
}

function findGlass(scene, start) {
  for (const pane of scene.meshes.filter((m) => m.metadata?.type === "carGlass")) {
    const state = pane.metadata.instance.panes.get(pane);
    if (state.crackTex) continue;
    pane.computeWorldMatrix(true);
    const bounds = pane.getBoundingInfo().boundingBox;
    const center = bounds.centerWorld;
    for (let a = 0; a < 16; a++) {
      const yaw = (a * Math.PI) / 8;
      for (const distance of [2, 3, 4]) {
        const x = center.x - Math.sin(yaw) * distance;
        const z = center.z - Math.cos(yaw) * distance;
        if (Math.abs(x) > 15.2 || Math.abs(z) > 15.2) continue;
        const origin = new Vector3(x, 1.7, z);
        const direction = center.subtract(origin).normalize();
        const ray = new Ray(origin, direction, 1000);
        const pick = scene.pickWithRay(ray);
        if (!pick?.hit || pick.pickedMesh !== pane) continue;
        pane.isPickable = false;
        const behind = scene.pickWithRay(ray);
        pane.isPickable = true;
        if (!behind?.hit || behind.pickedMesh.metadata?.type === "carGlass") continue;
        const pitch = -Math.atan2(direction.y, Math.hypot(direction.x, direction.z));
        return { ...start, player: [x, 0, z], yaw, pitch };
      }
    }
  }
  throw new Error("No repeatable line through a tempered window.");
}

export function capture({ scenario, expected }) {
  const g = window.__game;
  const dt = 1 / 60;
  g.engine.stopRenderLoop();
  g.startLoop = () => {};
  const input = g.input;
  const start = scenario.name === "glass" && !expected ? findGlass(g.scene, scenario.start) : scenario.start;
  const record = [],
    draws = [],
    pictures = [];
  const gameplay = new Map();
  const cosmetic = generator(start.seed ^ 0x9e3779b9);
  const random = Math.random;
  const shotRay = g.scene.pickWithRay.bind(g.scene);
  const frame = () => {
    g.engine.beginFrame();
    g.frame(false);
    g.engine.endFrame();
  };
  let tick = 0,
    real = 0,
    nextDraw = 0;
  const hits = [],
    reasons = [],
    events = [];
  const deadBefore = () => g.player.isDead;
  Math.random = cosmetic;
  globalThis.__fodGameplayDraw = (name) => {
    const site =
      name === "death.variant"
        ? new Error().stack.includes("DeathCam.begin")
          ? "player.death.variant"
          : "rival.death.variant"
        : name;
    if (expected) {
      const wanted = expected[nextDraw++];
      if (!wanted || wanted[0] !== tick || wanted[1] !== site)
        throw new Error(`Browser RNG divergence at ${tick} ${site} vs ${wanted}`);
      draws.push(wanted);
      return wanted[2];
    }
    if (!gameplay.has(site)) {
      let hash = start.seed;
      for (const ch of site) hash = Math.imul(hash ^ ch.charCodeAt(0), 0x01000193);
      gameplay.set(site, generator(hash));
    }
    const value = gameplay.get(site)();
    draws.push([tick, site, value]);
    return value;
  };
  const unsubscribe = ["playerDeath", "kill"].map((event) =>
    MatchEvents.on(event, (detail) => {
      events.push([event, detail?.weapon ?? null]);
    })
  );
  g.scene.pickWithRay = (...args) => {
    const result = shotRay(...args);
    if (args[0]?.length === 1000 && new Error().stack.includes("fireHitscanRay")) {
      const mesh = result?.pickedMesh;
      const type = mesh?.metadata?.type;
      const kind = type === "bot" ? "rival" : type === "carGlass" ? "glass" : mesh ? "world" : "miss";
      hits.push([kind, mesh?.name ?? null, mesh?.metadata?.zoneMult ?? null]);
    }
    return result;
  };
  for (const [method, name] of [
    ["playGlassCrackSound", "glass-hit"],
    ["playGlassBreakSound", "glass-break"],
    ["playBoltCycleSound", "bolt"],
    ["playUavOnlineSound", "uav"],
  ]) {
    const original = g.effects[method]?.bind(g.effects);
    if (original)
      g.effects[method] = (...args) => {
        reasons.push(name);
        return original(...args);
      };
  }
  try {
    g.botManager.setDifficultyLevel(start.difficulty);
    g.beginMatch(true);
    input.isPointerLocked = true;
    const bot = g.botManager.bots[0];
    g.player.position.set(...start.player);
    g.player.yaw = start.yaw;
    g.player.pitch = start.pitch;
    bot.reset(start.rival[0], start.rival[2], start.rivalYaw);
    bot.position.y = start.rival[1];
    if (start.flags & 1) bot.update = () => {};
    const updateBots = g.botManager.update.bind(g.botManager);
    if (start.deathTick) {
      g.player.invulnUntil = 0;
      g.botManager.update = (...args) => {
        updateBots(...args);
        if (tick === start.deathTick) g.player.takeDamage(1000, bot.position);
      };
    }
    g.weaponManager.activeIndex = start.weapon;
    if (start.clip >= 0) g.weaponManager.getActiveWeapon().clipAmmo = start.clip;
    g.botManager.playerKills = start.score[0];
    g.player.deaths = start.score[1];
    g.killstreaks.streak = start.streak;
    g.killstreaks.airstrikeReady = Boolean(start.flags & 2);
    g.killstreaks.apacheReady = Boolean(start.flags & 4);
    const actions = [
      [4, "ShiftLeft"],
      [8, "KeyZ"],
      [16, "KeyC"],
      [32, "KeyR"],
      [64, "KeyX"],
      [128, "Digit4"],
    ];
    const panes = g.scene.meshes.filter((mesh) => mesh.metadata?.type === "carGlass");
    for (const [moveX, moveY, lx, ly, held, pressed] of scenario.inputs) {
      tick++;
      real += dt;
      hits.length = reasons.length = events.length = 0;
      const previousDead = deadBefore();
      const previousMatch = g.matchState;
      input.mouseDeltaX = lx / g.player.lookSensitivity;
      input.mouseDeltaY = ly / g.player.lookSensitivity;
      for (const [key, active] of [
        ["KeyA", moveX < 0],
        ["KeyD", moveX > 0],
        ["KeyW", moveY > 0],
        ["KeyS", moveY < 0],
      ])
        input.keys.set(key, active);
      for (const [mask, key] of actions) {
        input.keys.set(key, Boolean(held & mask));
        if (pressed & mask) input.keysPressedThisFrame.add(key);
      }
      input.mouseButtons.set(0, Boolean(held & 1));
      input.mouseButtons.set(2, Boolean(held & 2));
      if (pressed & 1) input.mouseButtonsPressedThisFrame.add(0);
      g.time.frameDeltaTime = dt;
      const scaled = dt * g.time.scale;
      g.time.deltaTime = scaled;
      // Advance Babylon's skeletal clips on simulation time rather than the
      // automation machine's render or wall clock. A drawing checkpoint skips
      // animation evaluation and can be omitted without changing gameplay.
      g.scene._animate(scaled * 1000);
      const previousWeapon = g.weaponManager.getActiveWeapon();
      const previousClip = previousWeapon.clipAmmo;
      const previousState = previousWeapon.state;
      const previousDevice = g.killstreaks.handheldOut;
      const previousApache = g.killstreaks.apache.active;
      const previousBombs = g.killstreaks.bombs.length;
      frame();
      const weapon = g.weaponManager.getActiveWeapon();
      const device = g.killstreaks.transmitter.isOut ? 2 : g.killstreaks.laptop.isVisible ? 1 : 0;
      const p = g.player;
      if (g.weaponManager.firedThisFrame) events.push(["fire"]);
      if (weapon !== previousWeapon) events.push(["swap"]);
      if (weapon === previousWeapon && weapon.state === "reloading" && previousState !== "reloading")
        events.push(["reload-start"]);
      if (weapon === previousWeapon && weapon.state !== "reloading" && previousState === "reloading") events.push(["reload-end"]);
      if (weapon === previousWeapon && previousClip < weapon.clipAmmo && weapon.state === "reloading")
        events.push(["reload-round"]);
      if (previousState !== "cycling" && weapon.state === "cycling") events.push(["bolt-start"]);
      if (previousState === "cycling" && weapon.state !== "cycling") events.push(["bolt-end"]);
      if (!previousDevice && device === 1) events.push(["laptop-open"]);
      if (previousBombs === 0 && g.killstreaks.bombs.length > 0) events.push(["strike-mark"]);
      if (!previousApache && g.killstreaks.apache.active) events.push(["apache-launch"]);
      if (previousDead && !p.isDead) events.push(["respawn"]);
      if (previousMatch === "playing" && g.matchState === "ended") events.push(["match-end"]);
      record.push({
        tick,
        real,
        scaled,
        scale: g.time.scale,
        position: p.position.asArray(),
        rival: bot.position.asArray(),
        yaw: p.yaw,
        pitch: p.pitch,
        eye: p.eyeHeight,
        hp: p.health,
        rivalHp: bot.health,
        gun: g.weaponManager.activeIndex,
        gunState: weapon.state === "cycling" ? 2 : weapon.state === "reloading" ? 3 : 0,
        clip: weapon.clipAmmo,
        reserve: weapon.reserveAmmo,
        score: [g.botManager.playerKills, p.deaths],
        dead: Number(p.isDead),
        glass: panes.reduce((acc, mesh, i) => acc | (!mesh.isPickable ? 1 << i : 0), 0),
        device,
        apache: Number(g.killstreaks.apache.active),
        apachePosition: g.killstreaks.apache.root.position.asArray(),
        camera: g.cameraRig.camera.position.asArray(),
        cameraRotation: g.cameraRig.camera.rotation.asArray(),
        events: [...events],
        hits: [...hits],
        effects: [...reasons],
      });
      if (scenario.pictures.includes(tick)) {
        g.scene.render(true, true);
        if (!expected) pictures.push([tick, g.canvas.toDataURL("image/png").split(",")[1]]);
      }
    }
    if (scenario.name === "match-point" && (g.botManager.playerKills !== 10 || g.matchState !== "ended"))
      throw new Error("The match-point shot must close the browser duel.");
    if (expected && nextDraw !== expected.length) throw new Error(`Browser left ${expected.length - nextDraw} RNG draws unused`);
    g.scene.render(true, true);
    return { start, draws, ticks: record, pictures };
  } finally {
    Math.random = random;
    delete globalThis.__fodGameplayDraw;
    for (const off of unsubscribe) off();
  }
}
