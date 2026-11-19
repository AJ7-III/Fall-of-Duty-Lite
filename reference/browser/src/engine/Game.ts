import { Engine } from "@babylonjs/core/Engines/engine";
import { Scene } from "@babylonjs/core/scene";
import { Color4 } from "@babylonjs/core/Maths/math.color";
import { Time } from "./Time";
import { Input } from "./Input";
import { WorldMaterials } from "../world/materials/WorldMaterials";
import { PostProcessing } from "../rendering/PostProcessing";
import { resetDynamicShadowCasters } from "../rendering/dynamicShadows";
import { resetEnvironmentCache } from "../rendering/materials/environment";
import { CameraRig } from "../player/CameraRig";
import { PlayerController } from "../player/PlayerController";
import { DeathCam } from "../player/DeathCam";
import { WeaponManager } from "../weapons/WeaponManager";
import { ShipBoxMap } from "../world/ShipBoxMap";
import { BotManager } from "../bots/BotManager";
import { preloadSoldierModel, whenSoldierModelReady } from "../bots/SoldierAssets";
import type { SoldierDownload } from "../assets/soldierModel";
import { Killstreaks } from "../killstreaks/Killstreaks";
import { ViewModelRig } from "../rendering/ViewModelRig";
import { ScopeOverlay } from "../rendering/ScopeOverlay";
import { Effects } from "../rendering/Effects";
import { Hud } from "../ui/Hud";
import { Minimap } from "../ui/Minimap";
import { MatchUI } from "../ui/MatchUI";
import type { MatchResult, ScoreLine } from "../ui/MatchUI";
import { RIVALS, isFreeForAll, rivalById, setOpponents } from "../ui/MatchNames";
import { Settings } from "../ui/Settings";
import { RivalVoice } from "../ui/RivalVoice";
import { MatchEvents } from "../ui/MatchEvents";

type MatchState = "start" | "playing" | "paused" | "ended";

export class Game {
  // First to this many kills wins the match
  private static readonly KILL_LIMIT = 10;
  private scoreLines: ScoreLine[] = []; // this match's rivals, scores refreshed in place

  private canvas: HTMLCanvasElement;
  private engine: Engine;
  private scene: Scene;

  private time: Time;
  private input: Input;
  private materials: WorldMaterials;
  private postfx: PostProcessing;

  private cameraRig: CameraRig;
  private player: PlayerController;
  private weaponManager: WeaponManager;
  private map: ShipBoxMap;
  private botManager: BotManager;

  private viewModelRig: ViewModelRig;
  private scopeOverlay: ScopeOverlay;
  private effects: Effects;
  private hud: Hud;
  private minimap: Minimap;
  private matchUI: MatchUI;
  private rivalVoice: RivalVoice;
  private killstreaks: Killstreaks;
  private deathCam: DeathCam;

  // Gameplay runs only while playing. Menus render on changes, then leave
  // the last canvas frame underneath their DOM overlays.
  private matchState: MatchState = "start";
  private everLocked = false;
  private wasDead = false; // edge detector for the death-cam handoff
  private elapsed = 0; // monotonic game time for UI pulse phases
  private lastFpsUpdate = 0; // Time.fpsUpdates seen by the adaptive quality step-down
  private minimapAccumulator = 1 / 20;
  private hudRootEl = document.getElementById("hud-root");
  private lastHideCrosshair = false;
  private disposed = false;
  private assetsReady = false;
  public readonly assetsLoaded: Promise<void>;
  public readonly ready: Promise<void>;

  constructor(canvasId: string, soldierDownload?: SoldierDownload) {
    this.canvas = document.getElementById(canvasId) as HTMLCanvasElement;
    if (!this.canvas) {
      throw new Error(`Canvas with id ${canvasId} not found.`);
    }

    // 1. Initialize Babylon Engine and Scene
    // - antialias false: AA comes from the FXAA pass; a multisampled
    //   backbuffer behind a post-process pipeline only burns bandwidth
    // - preserveDrawingBuffer/stencil false: nothing reads pixels back and
    //   nothing uses stencil — both cost real GPU time when enabled
    // PostProcessing owns device-pixel scaling and the Fast pixel budget.
    // Engine auto-adaptation would multiply that scale again on DPR changes
    // (browser zoom / moving between monitors), bypassing the quality cap.
    this.engine = new Engine(this.canvas, false, { preserveDrawingBuffer: false, stencil: false });
    this.engine.renderEvenInBackground = false;
    let soldierReady: Promise<void> | undefined;
    try {
      this.scene = new Scene(this.engine);

      // Gritty industrial fog/clear color
      this.scene.clearColor = new Color4(0.08, 0.09, 0.11, 1.0);

      // FPS input is read from raw DOM events; without this Babylon raycasts
      // the whole scene on every pointermove (hundreds/sec on fast mice) just
      // to track the mesh under the cursor
      this.scene.skipPointerMovePicking = true;

      // The shared rigged-soldier asset loads in the background; soldier
      // bodies are built headless and grow their skin when this lands
      soldierReady = preloadSoldierModel(this.scene, soldierDownload);

      // 2. Initialize Subsystems
      this.time = new Time();
      this.input = new Input(this.canvas);
      this.materials = new WorldMaterials(this.scene);

      this.cameraRig = new CameraRig(this.scene);
      this.player = new PlayerController(this.input, this.cameraRig);
      this.weaponManager = new WeaponManager();
      this.map = new ShipBoxMap(this.scene, this.materials);

      this.viewModelRig = new ViewModelRig(this.scene, this.cameraRig);
      this.scopeOverlay = new ScopeOverlay();
      this.effects = new Effects(this.scene);
      // After the map: the bots grow their nav graph from its collision boxes.
      // Before the material freeze below: their materials are final too.
      this.botManager = new BotManager(this.scene, this.player, this.effects);
      // Respawns land far from (and hidden from) living enemies
      this.player.spawnPicker = (spawns) => this.botManager.pickPlayerSpawn(spawns);
      this.hud = new Hud(this.canvas);
      this.minimap = new Minimap();
      this.matchUI = new MatchUI(this.effects, () => this.weaponManager.getActiveWeapon().id, {
        onStart: () => this.startMatch(),
        onResume: () => this.input.requestPointerLock(),
        onEndMatch: () => this.endMatch(),
        onPlayAgain: () => this.restartMatch(),
        onDifficultyChange: (level) => {
          this.botManager.setDifficultyLevel(level);
          this.startLoop();
        },
        onToggleTrashTalk: (muted) => this.rivalVoice.setMuted(muted),
        onGraphicsChange: (quality) => this.postfx.setQuality(quality),
        onPreviewRival: (choice) =>
          this.rivalVoice.preview(choice === "ffa" ? RIVALS[Math.floor(Math.random() * RIVALS.length)] : rivalById(choice)),
      });
      this.rivalVoice = new RivalVoice();
      // Both build meshes/materials, so they sit before the freeze below: the
      // death-cam corpse actor, and the killstreak hardware (laptop viewmodel,
      // Apache airframe, the strike jets)
      this.deathCam = new DeathCam(this.scene, (scale) => {
        this.time.scale = scale;
      });
      this.killstreaks = new Killstreaks(
        this.scene,
        this.cameraRig.camera,
        this.input,
        this.player,
        this.botManager,
        this.effects,
        this.cameraRig
      );

      // Pause rides the pointer lock: P (or Escape) releases the lock, and ANY
      // unlock while playing opens the menu — there is no unpaused-but-unlocked
      // state to get stranded in. Re-locking from the Resume button unpauses.
      document.addEventListener("pointerlockchange", this.onPointerLockChange);

      // Post-processing: anti-aliasing, filmic tone mapping, sharpening,
      // bloom, vignette. This is what lifts the flat-shaded look into
      // something photographic.
      this.postfx = new PostProcessing(this.engine, this.scene, this.cameraRig.camera);
      this.postfx.setOnQualityChange((quality, auto) => {
        this.matchUI.renderGraphics(quality);
        this.map.setGraphicsQuality(quality);
        this.startLoop();
        if (auto)
          this.matchUI.toast(`FRAME RATE LOW — GRAPHICS SET TO ${quality === "performance" ? "FAST" : quality.toUpperCase()}`);
      });
      this.map.setGraphicsQuality(this.postfx.currentQuality);

      // Every material in the scene is now final: textures may still repaint
      // (target boards) and light uniforms still update (muzzle flash), but no
      // material ever gains/loses a texture slot or light. Freezing skips the
      // per-frame shader-define re-evaluation for every mesh.
      for (const material of this.scene.materials) {
        material.freeze();
      }

      this.setSimulationPaused(true);
      this.viewModelRig.setHidden(true);
      this.hud.hidePrompt();
      this.matchUI.showStart(this.botManager.getDifficultyLevel());

      // 3. Start Main Loop
      // Assets can finish loading after an idle frame has already been drawn.
      this.scene.onDataLoadedObservable.add(() => this.startLoop());
      whenSoldierModelReady(this.scene, () => this.startLoop());
      this.startLoop();

      this.assetsLoaded = soldierReady;
      this.ready = this.assetsLoaded.then(async () => {
        if (this.disposed) return;
        await this.scene.whenReadyAsync();
        if (!this.disposed) this.assetsReady = true;
      });

      // 4. Handle Window Resize
      window.addEventListener("resize", this.onResize);
      window.addEventListener("blur", this.onBlur);
      window.addEventListener("focus", this.onFocus);
      document.addEventListener("visibilitychange", this.onVisibilityChange);
    } catch (error) {
      this.dispose();
      // No Game instance is returned to await this load after a constructor
      // failure. Disposal may abort it and reject the pending request.
      void soldierReady?.catch(() => {});
      throw error;
    }
  }

  private onPointerLockChange = (): void => {
    const locked = document.pointerLockElement === this.canvas;
    if (locked) {
      // A pending capture can finish after the window has lost focus.
      if (document.hidden || !document.hasFocus()) {
        this.pauseMatch();
        document.exitPointerLock();
        return;
      }
      this.everLocked = true;
      if (this.matchState === "paused") this.resumeMatch();
    } else if (this.matchState === "playing" && this.everLocked) {
      this.pauseMatch();
    }
  };

  private onResize = (): void => {
    this.postfx.resize();
    this.startLoop();
  };

  private onBlur = (): void => {
    this.pauseMatch();
    if (document.pointerLockElement === this.canvas) document.exitPointerLock();
    // Babylon skips frame callbacks while unfocused, so the idle callback
    // cannot stop itself until focus returns.
    this.engine.stopRenderLoop();
  };

  private onFocus = (): void => this.startLoop();

  private onVisibilityChange = (): void => {
    if (document.hidden) {
      this.pauseMatch();
      if (document.pointerLockElement === this.canvas) document.exitPointerLock();
      this.engine.stopRenderLoop();
    } else {
      this.startLoop();
    }
  };

  public dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.engine.stopRenderLoop();
    document.removeEventListener("pointerlockchange", this.onPointerLockChange);
    window.removeEventListener("resize", this.onResize);
    window.removeEventListener("blur", this.onBlur);
    window.removeEventListener("focus", this.onFocus);
    document.removeEventListener("visibilitychange", this.onVisibilityChange);
    if (document.pointerLockElement === this.canvas) document.exitPointerLock();
    this.killstreaks?.dispose();
    this.rivalVoice?.dispose();
    this.matchUI?.dispose();
    this.hud?.dispose();
    this.effects?.dispose();
    this.postfx?.dispose();
    resetDynamicShadowCasters();
    resetEnvironmentCache();
    MatchEvents.clear();
    this.input?.dispose();
    this.scene?.dispose();
    this.engine.dispose();
  }

  private startLoop(): void {
    if (this.disposed || document.hidden) return;
    this.engine.maxFPS = this.matchState === "playing" ? this.postfx.frameRateLimit : 15;
    this.engine.performanceMonitor.reset();
    this.time.reset();
    if (this.matchState === "playing") this.postfx.resetPerformanceSamples();
    // Stable callback identity also makes resize/asset notifications idempotent.
    this.engine.runRenderLoop(this.renderFrame);
  }

  private renderFrame = (): void => {
    if (this.disposed || document.hidden) {
      this.engine.stopRenderLoop(this.renderFrame);
      return;
    }
    this.time.update();
    this.frame();
    // Allow asynchronous shaders/textures to finish before the final menu
    // frame, then stop scheduling GPU work until something changes.
    if (this.matchState !== "playing" && this.scene.isReady()) {
      this.engine.stopRenderLoop(this.renderFrame);
    }
  };

  private setSimulationPaused(paused: boolean): void {
    this.scene.animationsEnabled = !paused;
    this.map.setPaused(paused);
  }

  // Deterministic stepping for tooling: advance the simulation N frames of
  // exactly `dt` seconds each, rendering every one by default. Dev-only (see main.ts);
  // lets the death choreography, reload animations and bot behaviour be
  // inspected frame by frame from the console without a live mouse. Tests can
  // skip drawing intermediate frames while advancing the same update loop.
  public stepFrames(frames: number, dt: number, render = true): void {
    for (let i = 0; i < frames; i++) {
      this.time.deltaTime = dt;
      this.time.frameDeltaTime = dt;
      this.engine.beginFrame();
      this.frame(render);
      this.engine.endFrame();
    }
  }

  // One simulation + render frame. Time.deltaTime has already been set.
  private frame(render = true): void {
    if (this.matchState === "playing") {
      const dt = this.time.deltaTime;
      // A shot spawned during this update gets a full first frame, even after a hitch.
      this.effects.updateTransients(dt);

      // P releases the pointer lock; the pointerlockchange handler turns
      // that into the pause (same path Escape takes)
      if (this.input.isKeyPressed("KeyP") && this.input.getIsPointerLocked()) {
        document.exitPointerLock();
      }

      this.elapsed += dt;
      const activeWeapon = this.weaponManager.getActiveWeapon();
      const adsState = activeWeapon.adsAnimator.getInterpolatedState();

      // Step 2: Update Player (look, movement, camera rig)
      this.player.update(dt, adsState.sensitivityMultiplier, activeWeapon.adsAnimator.getProgress());
      // Refill before accepting the first frame's weapon input after respawn.
      if (this.player.consumeRespawn()) {
        this.weaponManager.refillAll();
        this.deathCam.end();
        this.wasDead = false;
      }

      // Step 3: Killstreaks — runs before weapons so opening the laptop or
      // transmitter cancels active reload/ADS and disconnects the trigger immediately.
      const wasHandheldOut = this.killstreaks.handheldOut;
      this.killstreaks.update(dt);
      if (!wasHandheldOut && this.killstreaks.handheldOut) {
        this.weaponManager.cancelActiveReload();
      }

      // Step 3.5: Update Weapon Systems (firing, bolt action state, reloading,
      // ADS animation). While killstreak hardware is in hand, LMB belongs
      // to that device — the trigger is disconnected.
      this.weaponManager.update(
        dt,
        this.input,
        this.player,
        this.cameraRig,
        this.scene,
        this.effects,
        !this.killstreaks.handheldOut
      );

      // Step 3.6: Bots perceive, decide, move and fight — after the player's
      // shots have landed (deaths register before they act) and with this
      // frame's gunshot noise for their hearing. A respawned player gets a
      // fresh Fall of Duty loadout.
      this.botManager.update(dt, this.player, this.weaponManager.firedThisFrame);

      // Step 3.7: Death: the moment the player drops (bot fire above, or
      // their own airstrike), the camera leaves their eyes and the corpse
      // actor takes the stage until respawn hands control back.
      if (this.player.isDead && !this.wasDead) {
        this.wasDead = true;
        this.deathCam.begin(this.player, this.cameraRig.camera, this.effects);
      }
      if (this.player.isDead) {
        this.deathCam.update(dt, this.cameraRig.camera);
      }

      // Step 4: Update ViewModel & Scope overlay rendering.
      // Re-fetch the weapon: the X-key swap may have just changed it.
      // Dead hands and killstreak hardware hands carry no rifle.
      const hideViewmodel = this.player.isDead || this.killstreaks.handheldOut;
      this.viewModelRig.setHidden(hideViewmodel);
      if (hideViewmodel) {
        this.scopeOverlay.update(0, 0, 0);
      } else {
        const shownWeapon = this.weaponManager.getActiveWeapon();
        const shownAds = shownWeapon.adsAnimator.getInterpolatedState();
        this.viewModelRig.update(
          dt,
          shownWeapon,
          this.input,
          this.player.isSprinting,
          this.weaponManager.getLowerAmount(),
          this.time.frameDeltaTime
        );
        this.scopeOverlay.update(shownAds.scopeOpacity, shownAds.vignetteOpacity, shownWeapon.adsAnimator.getProgress());
      }
      if (hideViewmodel !== this.lastHideCrosshair) {
        this.lastHideCrosshair = hideViewmodel;
        this.hudRootEl?.classList.toggle("hide-crosshair", hideViewmodel);
      }

      // Step 5: Update Map targets
      this.map.update(dt);

      // Step 5.5: Scoreboard + the first-to-10 win condition
      const scores = this.refreshScores();
      this.matchUI.setScore(this.botManager.playerKills, scores);
      if (this.botManager.playerKills >= Game.KILL_LIMIT) {
        this.endMatch("victory");
      } else if (Game.topScore(scores) >= Game.KILL_LIMIT) {
        this.endMatch("defeat");
      }
    }

    // Step 6: Render the current scene (idle calls redraw menu backgrounds).
    if (render) this.scene.render();

    // Step 6.5: Once a second while playing, let the image pipeline judge
    // whether the chosen quality tier is holding a playable frame rate
    if (this.matchState === "playing" && this.time.fpsUpdates !== this.lastFpsUpdate) {
      this.lastFpsUpdate = this.time.fpsUpdates;
      this.postfx.reportFps(this.time.fps);
    }

    // Step 7: Update HUD interface + minimap radar (UAV reveal included)
    if (this.matchState === "playing") {
      this.hud.update(this.weaponManager.getActiveWeapon(), this.input, this.player);
      const minimapInterval = this.postfx.currentQuality === "performance" ? 1 / 20 : 1 / 30;
      this.minimapAccumulator += this.time.deltaTime;
      if (this.minimapAccumulator >= minimapInterval) {
        this.minimapAccumulator %= minimapInterval;
        this.minimap.update(
          this.player,
          this.botManager.bots,
          this.killstreaks.uavActive,
          this.elapsed,
          this.killstreaks.getApacheRadarContact()
        );
      }
    }

    // Step 8: Clear single-frame key transitions (every state — stale
    // presses from menu typing must not fire on resume)
    this.input.postUpdate();
  }

  // ---------------------------------------------------------- match flow

  private beginMatch(isRestart: boolean): void {
    // The opponent picker: one rival, or the free-for-all with both. Sets their
    // squad's tuning, names on the scoreboard, comms and voice for this match.
    this.scoreLines = setOpponents(Settings.getOpponentChoice()).map((r) => ({ id: r.id, name: r.name, score: 0 }));
    if (isRestart) {
      this.map.resetMatch();
      this.effects.resetMatch();
    }
    this.player.resetForMatch();
    this.botManager.resetMatch(this.player);
    this.weaponManager.resetLoadout();
    this.killstreaks.resetMatch();
    this.rivalVoice.resetMatch();
    this.rivalVoice.matchStart();
    this.deathCam.end();
    this.wasDead = false;
    this.everLocked = false;
    this.lastHideCrosshair = false;
    this.hudRootEl?.classList.remove("hide-crosshair");
    this.matchUI.resetMatch();
    this.input.clearAllInputs();
    this.setSimulationPaused(false);
    this.effects.setAudioSuspended(false);
    this.killstreaks.setPaused(false);
    this.viewModelRig.setHidden(false);
    this.matchState = "playing";
    this.minimapAccumulator = 1 / 20;
    this.startLoop();
    this.input.requestPointerLock();
  }

  private startMatch(): void {
    if (this.matchState !== "start" || !this.assetsReady) return;
    this.beginMatch(false);
  }

  private pauseMatch(): void {
    if (this.matchState !== "playing") return;
    this.matchState = "paused";
    this.setSimulationPaused(true);
    this.effects.setAudioSuspended(true);
    this.rivalVoice.setPaused(true);
    this.input.clearAllInputs();
    this.player.resetInputLatch();
    this.killstreaks.setPaused(true); // the rotor must not thump over the menu
    this.hud.hidePrompt();
    this.matchUI.showPause(this.botManager.playerKills, this.refreshScores(), this.botManager.getDifficultyLevel());
    this.startLoop();
  }

  private resumeMatch(): void {
    if (this.matchState !== "paused") return;
    this.matchState = "playing";
    this.setSimulationPaused(false);
    this.effects.setAudioSuspended(false);
    this.rivalVoice.setPaused(false);
    this.input.clearAllInputs();
    this.player.resetInputLatch();
    this.killstreaks.setPaused(false);
    this.matchUI.hidePause();
    this.minimapAccumulator = 1 / 20;
    this.startLoop();
  }

  // No result given (End Match button): judge by the current score
  private endMatch(result?: MatchResult): void {
    if (this.matchState === "ended") return;
    const kills = this.botManager.playerKills;
    const deaths = this.player.deaths;
    const scores = this.refreshScores();
    const top = Game.topScore(scores); // duel: the player's deaths
    const finalResult: MatchResult = result ?? (kills > top ? "victory" : top > kills ? "defeat" : "draw");
    this.matchState = "ended";
    this.deathCam.end();
    this.wasDead = false;
    this.setSimulationPaused(true);
    this.effects.setAudioSuspended(false); // unmute before victory/defeat sting when ending from pause
    if (document.pointerLockElement === this.canvas) document.exitPointerLock();
    this.input.clearAllInputs();
    this.killstreaks.onMatchEnd(); // silence the rotor, shut the laptop, free the keys
    this.hud.hidePrompt();
    this.matchUI.setScore(kills, scores);
    this.matchUI.showEnd(finalResult, kills, scores, deaths);
    this.startLoop();
  }

  private restartMatch(): void {
    this.beginMatch(true);
  }

  // The rivals' scores: in a duel the rival scores the player's deaths (as
  // before); in a free-for-all each rival scores their soldier's kills of the
  // player and of the other rival.
  private refreshScores(): readonly ScoreLine[] {
    const ffa = isFreeForAll();
    for (const line of this.scoreLines) line.score = ffa ? this.botManager.rivalScore(line.id) : this.player.deaths;
    return this.scoreLines;
  }

  private static topScore(scores: readonly ScoreLine[]): number {
    let top = 0;
    for (const line of scores) top = Math.max(top, line.score);
    return top;
  }
}
