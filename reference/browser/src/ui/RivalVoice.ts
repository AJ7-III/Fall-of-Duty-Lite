import { MatchEvents } from "./MatchEvents";
import { getRivals, rivalById } from "./MatchNames";
import type { Rival, RivalId } from "./MatchNames";
import { Settings } from "./Settings";

const MATCH_TALK_EVENTS = 10;
const VOICE_VOLUME = 0.602;

// Each rival's voice lines, bundled from src/assets/voice/<rival id>/ by Vite:
//   match-start-*    the opening line as the match goes live
//   taunt-kill-*     the rival was just killed, by the player or the other rival (frustration)
//   taunt-victory-*  the rival just killed the player or the other rival (gloating)
const ALL_VOICE_CLIPS = import.meta.glob<string>("../assets/voice/*/*.{wav,mp3}", {
  eager: true,
  query: "?url",
  import: "default",
});
const clipsFor = (rival: RivalId, prefix: string): string[] =>
  Object.keys(ALL_VOICE_CLIPS)
    .filter((k) => k.includes(`/voice/${rival}/${prefix}`))
    .sort()
    .map((k) => ALL_VOICE_CLIPS[k]);

type VoiceChannel = { clips: readonly string[]; deck: string[]; gate: boolean[]; last: string | undefined };
type RivalChannels = Record<"kill" | "death", VoiceChannel>;
type Line = { src: string; speaker: Rival };

export class RivalVoice {
  private indicatorEl = document.getElementById("voice-comms");
  private nameEl = document.getElementById("voice-name");
  private voices = new Map<RivalId, RivalChannels>(); // this match's rivals
  private currentAudio: HTMLAudioElement | null = null;
  private queued: Line | null = null; // the next line, played as soon as the current one ends
  private playToken = 0;
  private muted = Settings.getTrashTalkMuted();
  private rivals: readonly Rival[] = getRivals();
  private unsubscribe: Array<() => void> = [];

  constructor() {
    this.resetMatch();

    // "kill" is the player scoring: that rival curses. "playerDeath" is a
    // rival scoring on the player: they gloat. "rivalKill" (free-for-all) is
    // one rival dropping the other: the loser curses, then the winner gloats.
    this.unsubscribe.push(MatchEvents.on("kill", (e) => this.maybePlay(e.victim, "kill")));
    this.unsubscribe.push(
      MatchEvents.on("playerDeath", (e) => {
        if (!e.self) this.maybePlay(e.killer ?? this.rivals[0].id, "death");
      })
    );
    this.unsubscribe.push(
      MatchEvents.on("rivalKill", (e) => {
        this.maybePlay(e.victim, "kill");
        this.maybePlay(e.killer, "death");
      })
    );
  }

  // Picks up this match's rivals (see setOpponents)
  public resetMatch(): void {
    this.stop();
    this.rivals = getRivals();
    if (this.nameEl) this.nameEl.textContent = this.rivals[0].name;
    this.voices.clear();
    for (const rival of this.rivals) {
      this.voices.set(rival.id, {
        kill: this.channel(clipsFor(rival.id, "taunt-kill-")),
        death: this.channel(clipsFor(rival.id, "taunt-victory-")),
      });
    }
  }

  // The opening line as the match goes live (one rival speaks for the yard)
  public matchStart(): void {
    if (this.muted) return;
    const rival = this.rivals[Math.floor(Math.random() * this.rivals.length)] ?? this.rivals[0];
    this.say(this.shuffled(clipsFor(rival.id, "match-start-")).pop(), rival);
  }

  public dispose(): void {
    this.stop();
    for (const off of this.unsubscribe) off();
    this.unsubscribe.length = 0;
  }

  // "Mute Trash Talk" toggle: silence the rival's voice comms and cut off
  // anything mid-sentence the moment it's switched on.
  public setMuted(muted: boolean): void {
    this.muted = muted;
    if (muted) this.stop();
  }

  // Pause menu: the clip is an HTMLAudioElement, outside the Web Audio context
  // Effects suspends, so the line has to be held here or it talks over the menu.
  public setPaused(paused: boolean): void {
    const audio = this.currentAudio;
    if (!audio) return;
    if (paused) audio.pause();
    else void audio.play().catch(() => this.stop());
  }

  // The opponent picker's voice check: one of their gloats, even with trash
  // talk muted, because the player asked for it.
  public preview(rival: Rival): void {
    this.stop();
    this.say(this.shuffled(clipsFor(rival.id, "taunt-victory-")).pop(), rival);
  }

  private maybePlay(id: RivalId, channel: "kill" | "death"): void {
    if (this.muted) return;
    const voice = this.voices.get(id)?.[channel];
    if (!voice) return;
    // They talk on about half their events. The gate refills because a
    // free-for-all can put a rival through more than MATCH_TALK_EVENTS.
    if (voice.gate.length === 0) voice.gate = this.shuffledTalkGate(MATCH_TALK_EVENTS);
    if (!voice.gate.pop()) return;
    if (this.currentAudio && this.queued) return; // one line already waiting its turn
    this.say(this.draw(voice), rivalById(id));
  }

  private channel(clips: readonly string[]): VoiceChannel {
    return { clips, deck: this.shuffled(clips), gate: this.shuffledTalkGate(MATCH_TALK_EVENTS), last: undefined };
  }

  // Next clip from the channel, reshuffling once every line has been heard
  // (without opening the new pass on the line that just closed the old one)
  private draw(voice: VoiceChannel): string | undefined {
    if (voice.deck.length === 0) {
      voice.deck = this.shuffled(voice.clips);
      const n = voice.deck.length;
      if (n > 1 && voice.deck[n - 1] === voice.last) [voice.deck[0], voice.deck[n - 1]] = [voice.deck[n - 1], voice.deck[0]];
    }
    voice.last = voice.deck.pop();
    return voice.last;
  }

  // Plays the line now, or straight after the one in progress
  private say(src: string | undefined, speaker: Rival): void {
    if (!src) return;
    if (this.currentAudio) {
      if (!this.queued) this.queued = { src, speaker };
      return;
    }
    if (this.nameEl) this.nameEl.textContent = speaker.name; // comms shows who is talking

    const audio = new Audio(src);
    audio.preload = "auto";
    audio.volume = VOICE_VOLUME;
    const token = ++this.playToken;

    const finish = (): void => {
      if (this.currentAudio !== audio || this.playToken !== token) return;
      this.currentAudio = null;
      audio.removeAttribute("src");
      audio.load();
      const next = this.queued;
      this.queued = null;
      if (next) this.say(next.src, next.speaker);
      else this.hide();
    };

    audio.addEventListener("ended", finish, { once: true });
    audio.addEventListener("error", finish, { once: true });
    audio.addEventListener("abort", finish, { once: true });
    this.currentAudio = audio;
    this.show();
    void audio.play().catch(finish);
  }

  private stop(): void {
    const audio = this.currentAudio;
    this.playToken++;
    this.currentAudio = null;
    this.queued = null;
    if (audio) {
      audio.pause();
      try {
        audio.currentTime = 0;
      } catch {
        // Some browsers reject seeks before metadata is ready.
      }
      audio.removeAttribute("src");
      audio.load();
    }
    this.hide();
  }

  private show(): void {
    this.indicatorEl?.classList.add("speaking");
    this.indicatorEl?.setAttribute("aria-hidden", "false");
  }

  private hide(): void {
    this.indicatorEl?.classList.remove("speaking");
    this.indicatorEl?.setAttribute("aria-hidden", "true");
  }

  private shuffled(urls: readonly string[]): string[] {
    const deck = [...urls];
    for (let i = deck.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [deck[i], deck[j]] = [deck[j], deck[i]];
    }
    return deck;
  }

  private shuffledTalkGate(eventCount: number): boolean[] {
    const gate: boolean[] = [];
    for (let i = 0; i < eventCount; i += 2) {
      if (Math.random() < 0.5) gate.push(true, false);
      else gate.push(false, true);
    }
    return gate.slice(0, eventCount).reverse();
  }
}
