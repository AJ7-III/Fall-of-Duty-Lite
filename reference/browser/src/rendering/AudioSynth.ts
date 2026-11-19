// Web Audio synthesis for every sound in the game: gunshots, UI chimes, the
// Apache rotor loop. 100% asset-free and independent of Babylon. Effects
// extends this, so callers keep a single `effects.playX()` surface.

type SoundEvent = readonly [delay: number, freq: number, duration: number, volume: number];

interface GunshotPreset {
  /** [freq, endFreq, sweepTime, gain, decayTime, stopTime] */
  noise: readonly [number, number, number, number, number, number];
  /** [freq, endFreq, sweepTime, gain, decayTime, stopTime] */
  osc: readonly [number, number, number, number, number, number];
  click?: SoundEvent;
}

type GunshotKind = "m40a3" | "usp45" | "mp44" | "apache";

const GUNSHOT_PRESETS: Record<GunshotKind, GunshotPreset> = {
  m40a3: { noise: [800, 10, 0.4, 0.8, 0.3, 0.5], osc: [120, 20, 0.15, 1.0, 0.2, 0.3] },
  usp45: { noise: [2600, 80, 0.18, 0.65, 0.16, 0.25], osc: [170, 35, 0.09, 0.75, 0.12, 0.15] },
  mp44: {
    noise: [1800, 55, 0.22, 0.58, 0.19, 0.28],
    osc: [135, 38, 0.1, 0.7, 0.14, 0.16],
    click: [0.018, 780, 0.035, 0.16],
  },
  apache: { noise: [1100, 60, 0.12, 0.5, 0.1, 0.16], osc: [105, 42, 0.06, 0.6, 0.09, 0.12] },
};

export class AudioSynth {
  private audioCtx: AudioContext | null = null;
  private masterOut: AudioNode | null = null;
  private audioSuspended = false;
  private audioListeners = new AbortController();

  // --- Apache rotor loop ---
  // One looping chopped-noise patch, started lazily, volume driven per-frame
  // by the helicopter's distance to the player and muted while paused.
  private rotorNodes: { src: AudioBufferSourceNode; lfo: OscillatorNode; gain: GainNode } | null = null;
  private rotorVol = 0;
  private rotorMuted = false;

  // Gunshot noise — generated once, reused for every shot (the buffer is
  // 24k random samples; rebuilding it per trigger pull is pure GC churn)
  private noiseBuffer: AudioBuffer | null = null;

  constructor() {
    // Lazy initialize AudioContext on first user interaction
    const initAudio = () => {
      if (this.audioCtx) {
        this.audioListeners.abort();
        return;
      }
      const AudioCtor =
        window.AudioContext || (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
      if (!AudioCtor) {
        this.audioListeners.abort();
        return;
      }
      try {
        this.audioCtx = new AudioCtor();
        this.audioListeners.abort();
        if (this.audioSuspended) {
          void this.audioCtx.suspend().catch(() => {});
        }
      } catch {
        // Keep listeners armed until a browser gesture permits AudioContext creation
      }
    };
    for (const evt of ["pointerdown", "mousedown", "click", "keydown"] as const) {
      window.addEventListener(evt, initAudio, { signal: this.audioListeners.signal });
    }
  }

  // Audio Synthesis via Web Audio API (100% legal, 100% asset-free)
  public setAudioSuspended(suspended: boolean): void {
    this.audioSuspended = suspended;
    if (!this.audioCtx) return;
    if (suspended) {
      if (this.audioCtx.state === "running") void this.audioCtx.suspend().catch(() => {});
    } else if (this.audioCtx.state === "suspended") {
      void this.audioCtx.resume().catch(() => {});
    }
  }

  private getAudioContext(): AudioContext | null {
    if (this.audioSuspended || !this.audioCtx) return null;
    if (this.audioCtx.state === "suspended") {
      void this.audioCtx.resume().catch(() => {});
    }
    return this.audioCtx;
  }

  private getOutput(ctx: AudioContext): AudioNode {
    if (!this.masterOut) {
      const comp = ctx.createDynamicsCompressor();
      comp.threshold.value = -14;
      comp.knee.value = 12;
      comp.ratio.value = 6;
      comp.attack.value = 0.003;
      comp.release.value = 0.15;
      comp.connect(ctx.destination);
      this.masterOut = comp;
    }
    return this.masterOut;
  }

  private getNoiseBuffer(ctx: AudioContext): AudioBuffer {
    if (!this.noiseBuffer) {
      const bufferSize = ctx.sampleRate * 0.5; // 0.5s duration
      this.noiseBuffer = ctx.createBuffer(1, bufferSize, ctx.sampleRate);
      const data = this.noiseBuffer.getChannelData(0);
      for (let i = 0; i < bufferSize; i++) {
        data[i] = Math.random() * 2 - 1;
      }
    }
    return this.noiseBuffer;
  }

  private playNoiseBurst(
    ctx: AudioContext,
    now: number,
    opts: {
      filterType: BiquadFilterType;
      freq: number;
      endFreq?: number;
      freqTime?: number;
      midFreq?: { freq: number; time: number };
      q?: number;
      gainStart?: number;
      gainPeak?: { value: number; time: number };
      gainEnd: number;
      gainDecayTime: number;
      stopTime: number;
      loop?: boolean;
      dest?: AudioNode;
    }
  ): void {
    const noise = ctx.createBufferSource();
    noise.buffer = this.getNoiseBuffer(ctx);
    if (opts.loop) noise.loop = true;

    const filter = ctx.createBiquadFilter();
    filter.type = opts.filterType;
    if (opts.q !== undefined) filter.Q.value = opts.q;
    filter.frequency.setValueAtTime(opts.freq, now);
    if (opts.midFreq) {
      filter.frequency.exponentialRampToValueAtTime(opts.midFreq.freq, now + opts.midFreq.time);
    }
    if (opts.endFreq !== undefined && opts.freqTime !== undefined) {
      filter.frequency.exponentialRampToValueAtTime(opts.endFreq, now + opts.freqTime);
    }

    const gain = ctx.createGain();
    gain.gain.setValueAtTime(Math.max(0.0001, opts.gainStart ?? 0.0001), now);
    if (opts.gainPeak) {
      gain.gain.exponentialRampToValueAtTime(Math.max(0.0001, opts.gainPeak.value), now + opts.gainPeak.time);
    }
    gain.gain.exponentialRampToValueAtTime(opts.gainEnd, now + opts.gainDecayTime);

    noise.connect(filter);
    filter.connect(gain);
    gain.connect(opts.dest ?? this.getOutput(ctx));
    noise.start(now);
    noise.stop(now + opts.stopTime);
  }

  private playTone(
    ctx: AudioContext,
    time: number,
    opts: {
      type?: OscillatorType;
      freq: number;
      endFreq?: number;
      sweepTime?: number;
      gainStart: number;
      attack?: { value: number; time: number };
      gainEnd: number;
      decayTime: number;
      stopTime: number;
      bandpass?: boolean;
      dest?: AudioNode;
    }
  ): void {
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = opts.type ?? "sine";
    osc.frequency.setValueAtTime(opts.freq, time);
    if (opts.endFreq !== undefined && opts.sweepTime !== undefined) {
      osc.frequency.exponentialRampToValueAtTime(opts.endFreq, time + opts.sweepTime);
    }

    gain.gain.setValueAtTime(Math.max(0.0001, opts.gainStart), time);
    if (opts.attack) {
      gain.gain.exponentialRampToValueAtTime(Math.max(0.0001, opts.attack.value), time + opts.attack.time);
    }
    gain.gain.exponentialRampToValueAtTime(opts.gainEnd, time + opts.decayTime);

    if (opts.bandpass) {
      const filter = ctx.createBiquadFilter();
      filter.type = "bandpass";
      filter.frequency.setValueAtTime(opts.freq, time);
      osc.connect(filter);
      filter.connect(gain);
    } else {
      osc.connect(gain);
    }
    gain.connect(opts.dest ?? this.getOutput(ctx));
    osc.start(time);
    osc.stop(time + opts.stopTime);
  }

  // Shared gunshot synth: every weapon's report is the same two-layer patch —
  // a lowpass-swept noise burst (the blast) over a pitch-dropping sine thump
  // (the body) — differing only in preset values. Both gains decay to 0.01.
  private playGunshot(cfg: GunshotPreset, volume = 1): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    const now = ctx.currentTime;
    const [freq, endFreq, freqTime, noiseGain, gainDecayTime, noiseStop] = cfg.noise;
    const [oscFreq, oscEnd, sweepTime, oscGain, oscDecay, oscStop] = cfg.osc;

    this.playNoiseBurst(ctx, now, {
      filterType: "lowpass",
      freq,
      endFreq,
      freqTime,
      gainStart: noiseGain * volume,
      gainEnd: 0.01,
      gainDecayTime,
      stopTime: noiseStop,
    });

    this.playTone(ctx, now, {
      type: "sine",
      freq: oscFreq,
      endFreq: oscEnd,
      sweepTime,
      gainStart: oscGain * volume,
      gainEnd: 0.01,
      decayTime: oscDecay,
      stopTime: oscStop,
    });

    if (cfg.click) {
      this.playClick(ctx, now + cfg.click[0], cfg.click[1], cfg.click[2], cfg.click[3] * volume);
    }
  }

  public playShootSound(volume: number = 1): void {
    this.playGunshot(GUNSHOT_PRESETS.m40a3, volume);
  }

  // Positional-ish report for bot fire: the same patch as the player's
  // weapon, scaled down with distance to the listener
  public playBotShootSound(weapon: "mp44" | "usp45" | "m40a3", distance: number): void {
    this.playGunshot(GUNSHOT_PRESETS[weapon], Math.min(1, 1.35 / (1 + distance * 0.12)));
  }

  // Supersonic snap of a near miss passing the player's head
  public playBulletWhizSound(): void {
    this.playClicks([0, 3400, 0.03, 0.12], [0.012, 2200, 0.045, 0.09]);
  }

  // Dull body thud layered under the screen blood when the player is hit
  public playPlayerHitSound(): void {
    this.playClicks([0, 210, 0.07, 0.5], [0.015, 95, 0.13, 0.55]);
  }

  public playBoltCycleSound(): void {
    this.playClicks([0, 800, 0.08, 0.3], [0.3, 600, 0.1, 0.4]); // bolt pulled back, then pushed forward
  }

  public playBoltOpenSound(): void {
    this.playClicks([0, 850, 0.07, 0.3], [0.1, 620, 0.09, 0.25]); // handle rotated up, bolt drawn back
  }

  public playRoundInsertSound(): void {
    this.playClicks([0, 950, 0.04, 0.22], [0.05, 480, 0.07, 0.3]); // round pressed in, follower spring clack
  }

  public playBoltCloseSound(): void {
    this.playClicks([0, 600, 0.1, 0.4], [0.12, 820, 0.08, 0.35]); // bolt run forward, handle locked down
  }

  private playClick(ctx: AudioContext, time: number, freq: number, duration: number, volume: number): void {
    this.playTone(ctx, time, {
      type: "triangle",
      freq,
      endFreq: freq / 2,
      sweepTime: duration,
      gainStart: volume,
      gainEnd: 0.001,
      decayTime: duration,
      stopTime: duration,
      bandpass: true,
    });
  }

  // Mechanical sounds are just 1-3 playClick()s at offsets from the trigger moment.
  private playClicks(...clicks: SoundEvent[]): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    const now = ctx.currentTime;
    for (const [delay, freq, duration, volume] of clicks) {
      this.playClick(ctx, now + delay, freq, duration, volume);
    }
  }

  // Sharper, shorter crack than the rifle: less low-end, faster decay
  public playPistolShootSound(volume: number = 1): void {
    this.playGunshot(GUNSHOT_PRESETS.usp45, volume);
  }

  // Automatic rifle report: sharper than the sniper, heavier than the pistol,
  // with a short mechanical slap layered in so full-auto has a stamped-metal cadence.
  public playMp44ShootSound(volume: number = 1): void {
    this.playGunshot(GUNSHOT_PRESETS.mp44, volume);
  }

  public playDryFireSound(): void {
    this.playClicks([0, 700, 0.05, 0.25]); // hammer falls on nothing
  }

  public playMagOutSound(): void {
    this.playClicks([0, 520, 0.06, 0.3], [0.07, 380, 0.06, 0.25]); // mag release pressed, mag slides free
  }

  public playMagInSound(): void {
    this.playClicks([0, 430, 0.06, 0.35], [0.06, 920, 0.05, 0.3]); // mag body seats, catch clicks over
  }

  public playSlideReleaseSound(): void {
    this.playClicks([0, 760, 0.05, 0.35], [0.04, 540, 0.09, 0.45]); // release lever, slide slams home
  }

  // Single sharp tick as a laminated pane stars around the bullet
  public playGlassCrackSound(): void {
    this.playClicks([0, 2600, 0.05, 0.3], [0.025, 3400, 0.04, 0.2]);
  }

  // Full shatter: bright noise burst + a cascade of descending tinks
  public playGlassBreakSound(): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    const now = ctx.currentTime;

    this.playNoiseBurst(ctx, now, {
      filterType: "highpass",
      freq: 2400,
      gainStart: 0.45,
      gainEnd: 0.01,
      gainDecayTime: 0.32,
      stopTime: 0.35,
    });

    this.playClick(ctx, now + 0.03, 3200, 0.06, 0.3);
    this.playClick(ctx, now + 0.09, 2300, 0.07, 0.26);
    this.playClick(ctx, now + 0.16, 1700, 0.08, 0.2);
  }

  public playWeaponSwitchSound(): void {
    this.playClicks([0, 320, 0.06, 0.16], [0.16, 640, 0.05, 0.18]); // gear rustle, next weapon settles
  }

  public playHitSound(): void {
    const ctx = this.getAudioContext();
    if (ctx) this.playTone(ctx, ctx.currentTime, { freq: 1800, gainStart: 0.2, gainEnd: 0.001, decayTime: 0.05, stopTime: 0.06 });
  }

  // --- Reward chimes (match UI) — clean sine notes, distinct from the percussive weapon clicks.
  private playTones(...notes: SoundEvent[]): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    const now = ctx.currentTime;
    for (const [delay, freq, duration, volume] of notes) {
      this.playTone(ctx, now + delay, {
        type: "sine",
        freq,
        gainStart: 0.0001,
        attack: { value: volume, time: 0.012 },
        gainEnd: 0.001,
        decayTime: duration,
        stopTime: duration + 0.02,
      });
    }
  }

  // Two quick rising notes on a kill; headshots ring a third, higher one
  public playKillSound(headshot: boolean): void {
    this.playTones([0, 880, 0.1, 0.16], [0.07, 1175, 0.14, 0.16]);
    if (headshot) this.playTones([0.15, 1568, 0.2, 0.15]);
  }

  // Short ascending fanfare for killstreak callouts — higher tiers reach higher
  public playStreakSound(tier: number): void {
    const notes: SoundEvent[] = [
      [0, 523, 0.12, 0.14],
      [0.09, 659, 0.12, 0.14],
      [0.18, 784, 0.2, 0.15],
    ];
    if (tier >= 2) notes.push([0.27, 1046, 0.26, 0.15]);
    this.playTones(...notes);
  }

  public playVictorySound(): void {
    this.playTones([0, 523, 0.18, 0.16], [0.14, 659, 0.18, 0.16], [0.28, 784, 0.18, 0.16], [0.42, 1046, 0.5, 0.18]);
  }

  public playDefeatSound(): void {
    this.playTones([0, 392, 0.3, 0.16], [0.22, 311, 0.3, 0.15], [0.44, 233, 0.55, 0.15]);
  }

  // Player down: original retro-arcade "invader crumple" chirp. Bright,
  // stepped square waves over a tiny noise zap so it reads cartoonish instead
  // of like the enemy body's human death gasp.
  public playPlayerDeathSound(): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    const now = ctx.currentTime;

    const master = ctx.createGain();
    master.gain.setValueAtTime(0.75, now);
    master.gain.exponentialRampToValueAtTime(0.001, now + 0.72);
    master.connect(this.getOutput(ctx));

    const filter = ctx.createBiquadFilter();
    filter.type = "lowpass";
    filter.frequency.setValueAtTime(2600, now);
    filter.frequency.exponentialRampToValueAtTime(520, now + 0.62);
    filter.connect(master);

    const tones: Array<[number, number, number]> = [
      [0, 880, 0.08],
      [0.07, 740, 0.08],
      [0.14, 622, 0.08],
      [0.21, 466, 0.1],
      [0.32, 330, 0.16],
    ];
    for (const [delay, freq, duration] of tones) {
      this.playTone(ctx, now + delay, {
        type: "square",
        freq,
        endFreq: Math.max(70, freq * 0.55),
        sweepTime: duration,
        gainStart: 0.0001,
        attack: { value: 0.16, time: 0.01 },
        gainEnd: 0.001,
        decayTime: duration,
        stopTime: duration + 0.02,
        dest: filter,
      });
    }

    this.playNoiseBurst(ctx, now, {
      filterType: "bandpass",
      q: 5,
      freq: 1500,
      endFreq: 320,
      freqTime: 0.45,
      gainStart: 0.12,
      gainEnd: 0.001,
      gainDecayTime: 0.42,
      stopTime: 0.45,
      dest: master,
    });
  }

  // --- Killstreak audio ---

  // Deep boom: looped noise (the 0.5s buffer is too short for the tail
  // one-shot) swept low, under a falling sub thump and a debris click
  public playExplosionSound(distance: number): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    const volume = Math.min(1, 2.4 / (1 + distance * 0.09));
    const now = ctx.currentTime;

    this.playNoiseBurst(ctx, now, {
      filterType: "lowpass",
      freq: 340,
      endFreq: 26,
      freqTime: 0.95,
      gainStart: 1.15 * volume,
      gainEnd: 0.01,
      gainDecayTime: 0.95,
      stopTime: 1.05,
      loop: true,
    });

    this.playTone(ctx, now, {
      type: "sine",
      freq: 78,
      endFreq: 22,
      sweepTime: 0.42,
      gainStart: 1.05 * volume,
      gainEnd: 0.01,
      decayTime: 0.55,
      stopTime: 0.6,
    });

    this.playClick(ctx, now + 0.16, 540, 0.09, 0.18 * volume);
  }

  // Fast mover overhead: a band-swept roar that swells, passes and recedes
  public playJetFlybySound(): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    this.playNoiseBurst(ctx, ctx.currentTime, {
      filterType: "bandpass",
      q: 1.1,
      freq: 340,
      midFreq: { freq: 1500, time: 0.8 },
      endFreq: 380,
      freqTime: 1.7,
      gainStart: 0.0001,
      gainPeak: { value: 0.5, time: 0.75 },
      gainEnd: 0.001,
      gainDecayTime: 1.75,
      stopTime: 1.8,
      loop: true,
    });
  }

  // Radar pings for the UAV coming online
  public playUavOnlineSound(): void {
    this.playTones([0, 1240, 0.09, 0.13], [0.22, 1240, 0.09, 0.1], [0.44, 1560, 0.3, 0.12]);
  }

  // Radio confirm for a called strike: squelch click + affirmative beeps
  public playAirstrikeConfirmSound(): void {
    this.playClicks([0, 1900, 0.025, 0.12]);
    this.playTones([0.06, 990, 0.08, 0.12], [0.17, 990, 0.08, 0.12], [0.3, 740, 0.16, 0.12]);
  }

  public playLaptopOpenSound(): void {
    this.playClicks([0, 480, 0.05, 0.2], [0.12, 720, 0.04, 0.18]);
    this.playTones([0.22, 1180, 0.12, 0.08]);
  }

  public playLaptopCloseSound(): void {
    this.playClicks([0, 700, 0.04, 0.18], [0.08, 420, 0.06, 0.2]);
  }

  // Chin gun: short bark per round, deeper than any infantry rifle
  public playApacheGunSound(volume: number): void {
    this.playGunshot(GUNSHOT_PRESETS.apache, volume);
  }

  // The dying breath: a swell of band-filtered air falling in pitch
  public playDeathGaspSound(volume: number): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    this.playNoiseBurst(ctx, ctx.currentTime, {
      filterType: "bandpass",
      q: 1.4,
      freq: 880,
      endFreq: 360,
      freqTime: 0.5,
      gainStart: 0.0001,
      gainPeak: { value: 0.27 * volume, time: 0.12 },
      gainEnd: 0.001,
      gainDecayTime: 0.55,
      stopTime: 0.6,
      loop: true,
    });
  }

  // Dead weight meeting the ground, gear clattering on top of it
  public playBodyFallSound(volume: number): void {
    this.playClicks([0, 150, 0.08, 0.5 * volume], [0.06, 95, 0.13, 0.55 * volume], [0.14, 330, 0.05, 0.2 * volume]);
  }

  // --- Apache rotor loop: started lazily, volume steered every frame ---

  private startRotorLoop(ctx: AudioContext): void {
    const src = ctx.createBufferSource();
    src.buffer = this.getNoiseBuffer(ctx);
    src.loop = true;
    const lowpass = ctx.createBiquadFilter();
    lowpass.type = "lowpass";
    lowpass.frequency.value = 240;
    const chop = ctx.createGain(); // blade-pass amplitude chop
    chop.gain.value = 0.55;
    const lfo = ctx.createOscillator();
    lfo.type = "square";
    lfo.frequency.value = 13;
    const lfoDepth = ctx.createGain();
    lfoDepth.gain.value = 0.4;
    lfo.connect(lfoDepth);
    lfoDepth.connect(chop.gain);
    const master = ctx.createGain();
    master.gain.value = 0;
    src.connect(lowpass);
    lowpass.connect(chop);
    chop.connect(master);
    master.connect(this.getOutput(ctx));
    src.start();
    lfo.start();
    this.rotorNodes = { src, lfo, gain: master };
  }

  public setRotorVolume(volume: number): void {
    const ctx = this.getAudioContext();
    if (!ctx) return;
    if (!this.rotorNodes) this.startRotorLoop(ctx);
    this.rotorVol = volume;
    this.rotorNodes!.gain.gain.setTargetAtTime(this.rotorMuted ? 0 : volume, ctx.currentTime, 0.12);
  }

  // Pause menu: the world freezes, the rotor must too
  public setRotorMuted(muted: boolean): void {
    this.rotorMuted = muted;
    if (this.rotorNodes && this.audioCtx) {
      this.rotorNodes.gain.gain.setTargetAtTime(muted ? 0 : this.rotorVol, this.audioCtx.currentTime, 0.08);
    }
  }

  public stopRotorLoop(): void {
    if (!this.rotorNodes) return;
    const { src, lfo, gain } = this.rotorNodes;
    this.rotorNodes = null;
    this.rotorVol = 0;
    // Fade out over a few ms rather than cut the waveform mid-cycle, which clicks.
    // A suspended or missing context is silent anyway, so stop at once there.
    const ctx = this.audioCtx;
    const running = ctx !== null && ctx.state === "running";
    if (running) gain.gain.setTargetAtTime(0, ctx.currentTime, 0.03);
    src.onended = () => {
      src.disconnect();
      lfo.disconnect();
      gain.disconnect();
    };
    try {
      const at = running ? ctx.currentTime + 0.15 : 0;
      src.stop(at);
      lfo.stop(at);
    } catch {
      // already stopped — fine
    }
  }

  protected disposeAudio(): void {
    this.audioListeners.abort();
    this.stopRotorLoop();
    this.masterOut?.disconnect();
    this.masterOut = null;
    if (this.audioCtx) void this.audioCtx.close().catch(() => {});
    this.audioCtx = null;
    this.noiseBuffer = null;
  }
}
