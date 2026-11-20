import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { crc32 } from "node:zlib";
import { encodeVoice } from "./ima-adpcm.mjs";

const root = fileURLToPath(new URL("..", import.meta.url));
const catalog = readFileSync(join(root, "campaign_dialogue.inc"), "utf8");
const lines = catalog.split("\n").filter((line) => line.startsWith("CAMPAIGN_LINE(")).map((line) => {
  const match = line.match(/^CAMPAIGN_LINE\((\w+), ("[^"]*"), ("[^"]*"), (Radio|Direct|SubtitleOnly)\)$/);
  if (!match) throw new Error(`Invalid dialogue row: ${line}`);
  return { id: match[1], speaker: JSON.parse(match[2]), text: JSON.parse(match[3]), profile: match[4] };
});
if (lines.at(-1)?.profile !== "SubtitleOnly" || lines.slice(0, -1).some((line) => line.profile === "SubtitleOnly")) {
  throw new Error("The subtitle-only fallback must be the last catalog row.");
}
const voiced = lines.slice(0, -1);
const mappingPath = resolve(process.argv[2] ?? join(root, ".voice-sources", "ingest.json"));
const sourceDir = resolve(process.argv[3] ?? join(dirname(mappingPath), "raw"));
const sources = JSON.parse(readFileSync(mappingPath, "utf8"));
const inputHashes = new Set();
const sha256 = (data) => createHash("sha256").update(data).digest("hex");
if (Object.keys(sources).length !== voiced.length) throw new Error("Ingest map and dialogue catalog disagree.");

const ffmpegPath = process.env.FFMPEG || "ffmpeg";
function decode(inputArgs, filters, rate, input) {
  const args = ["-v", "error", "-nostdin", ...inputArgs];
  if (filters.length) args.push("-af", filters.join(","));
  args.push("-ac", "1", "-ar", String(rate), "-f", "s16le", "-");
  const result = spawnSync(ffmpegPath, args, { input, maxBuffer: 4 * 1024 * 1024 });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(result.stderr.toString());
  return result.stdout;
}

// Find quiet edges in 10 ms windows. Leave room for soft attacks and consonant tails.
function trimEdges(pcm, rate) {
  const frames = pcm.length / 2;
  const window = rate / 100;
  const threshold = (32768 * 10 ** (-47 / 20)) ** 2;
  let first = frames;
  let last = 0;
  for (let start = 0; start < frames; start += window) {
    const end = Math.min(frames, start + window);
    let power = 0;
    for (let i = start; i < end; ++i) power += pcm.readInt16LE(i * 2) ** 2;
    if (power / (end - start) < threshold) continue;
    first = Math.min(first, start);
    last = end;
  }
  if (!last) throw new Error("The source contains no audible speech.");
  return pcm.subarray(Math.max(0, first - 5 * window) * 2, Math.min(frames, last + 9 * window) * 2);
}

function levelSpeech(pcm, rate) {
  const frames = pcm.length / 2;
  let peak = 0;
  let power = 0;
  let audible = 0;
  for (let i = 0; i < frames; ++i) {
    const magnitude = Math.abs(pcm.readInt16LE(i * 2));
    peak = Math.max(peak, magnitude);
    if (magnitude > 230) {
      power += magnitude ** 2;
      ++audible;
    }
  }
  if (!audible) throw new Error("The filtered clip contains no audible speech.");
  const target = 32768 * 10 ** (-19 / 20);
  const ceiling = 32768 * 10 ** (-4.5 / 20);
  const gain = Math.min(target / Math.sqrt(power / audible), ceiling / peak, 2);
  const fadeIn = Math.round(rate * .008);
  const fadeOut = Math.round(rate * .022);
  for (let i = 0; i < frames; ++i) {
    const fade = Math.min(1, i / fadeIn, (frames - i - 1) / fadeOut);
    pcm.writeInt16LE(Math.round(pcm.readInt16LE(i * 2) * gain * fade), i * 2);
  }
}

const sourceRate = 24000;
// Keep the clipped wake-up delivery brisk so the two early plasma beats stay close together.
const wakeTargetSeconds = { DeltaWakeUp: 1.94, DeltaWarhorseDown: 2.24 };
const table = Buffer.alloc(voiced.length * 12);
const codes = [];
const clips = [];
let offset = 0;
for (const [i, line] of voiced.entries()) {
  const { id, speaker, text, profile } = line;
  const source = sources[id];
  if (!source || !source.filename || /[/\\]/.test(source.filename) || source.filename === "..") {
    throw new Error(`Missing or invalid source filename for ${id}.`);
  }
  const path = join(sourceDir, source.filename);
  const hash = sha256(readFileSync(path));
  if (hash !== source.sha256 || inputHashes.has(hash)) throw new Error(`Changed or duplicate source for ${id}.`);
  inputHashes.add(hash);
  const trimmed = trimEdges(decode(["-i", path], [], sourceRate), sourceRate);
  const trimmedSeconds = trimmed.length / (sourceRate * 2);
  const rate = profile === "Radio" ? 12000 : sourceRate;
  const filters = profile === "Radio"
    ? ["highpass=f=300", "lowpass=f=3600", "acompressor=threshold=0.17:ratio=2:attack=8:release=85"]
    : ["highpass=f=75"];
  const tempo = wakeTargetSeconds[id] ? Math.min(1.18, trimmedSeconds / wakeTargetSeconds[id]) : 1;
  if (tempo > 1) filters.push(`atempo=${tempo.toFixed(5)}`);
  const pcm = decode(["-f", "s16le", "-ar", String(sourceRate), "-ac", "1", "-i", "-"], filters, rate, trimmed);
  levelSpeech(pcm, rate);
  const clip = encodeVoice(pcm);
  const divisor = sourceRate / rate;
  const entry = i * 12;
  table.writeUInt32LE(clip.frames, entry);
  table.writeUInt32LE(offset, entry + 4);
  table.writeInt16LE(clip.predictor, entry + 8);
  table[entry + 10] = clip.index;
  table[entry + 11] = divisor;
  codes.push(clip.codes);
  offset += clip.codes.length;
  clips.push({ id, speaker, text, profile, sampleRate: rate, frames: clip.frames, sourceSha256: hash });
}

const body = Buffer.concat([table, ...codes]);
const digest = createHash("sha256").update(catalog).update(body).digest();
const fingerprint = digest.readBigUInt64BE().toString(16).padStart(16, "0");
const header = Buffer.alloc(32);
header.write("FODV", "ascii");
header.writeUInt32LE(1, 8);
header.writeUInt32LE(clips.length, 12);
header.writeUInt32LE(header.length + body.length, 16);
header.writeUInt32LE(crc32(body), 20);
header.writeBigUInt64LE(BigInt(`0x${fingerprint}`), 24);
const bank = Buffer.concat([header, body]);
if (bank.length > 8 * 1024 * 1024) throw new Error("Campaign voice bank exceeds the runtime size limit.");
const manifest = { schema: 1, fingerprint, sha256: sha256(bank), storedBytes: bank.length, clips };
const timing = [
  "// Generated by tools/build-campaign-voices.mjs. Counts are at the 24 kHz output rate.",
  "#pragma once", "", '#include "campaign_dialogue.hpp"', "", "namespace fod {", "",
  `inline constexpr uint64_t campaignVoiceFingerprint = 0x${fingerprint}ULL;`,
  `inline constexpr char campaignVoiceBuildMarker[] = "FODV:${fingerprint}";`,
  "inline constexpr uint32_t campaignVoiceFrames[size_t(CampaignLine::Count)] = {",
  "    0, // None",
  ...clips.map(({ id, frames, sampleRate }) => `    ${frames * sourceRate / sampleRate}, // ${id}`),
  `    0, // ${lines.at(-1).id}`, "};", "", "} // namespace fod", "",
].join("\n");
for (const [path, data] of [
  ["assets/campaign-voices.fodv", bank],
  ["assets/campaign-voices.fodv.json", JSON.stringify(manifest, null, 2) + "\n"],
  ["campaign_voice_timing.hpp", timing],
]) {
  const destination = join(root, path);
  const bytes = Buffer.isBuffer(data) ? data : Buffer.from(data);
  if (existsSync(destination) && readFileSync(destination).equals(bytes)) continue;
  const temporary = destination + `.${process.pid}.tmp`;
  writeFileSync(temporary, bytes);
  renameSync(temporary, destination);
}
console.log(`Packed ${clips.length} campaign takes into ${bank.length.toLocaleString()} bytes (${fingerprint}).`);
