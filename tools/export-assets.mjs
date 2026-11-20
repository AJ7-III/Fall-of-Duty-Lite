import { chromium } from "@playwright/test";
import { createServer } from "vite";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdir, mkdtemp, readFile, rename, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { deflateSync } from "node:zlib";
import { encodeVoice, wav } from "./ima-adpcm.mjs";

const root = fileURLToPath(new URL("../", import.meta.url));
const { values } = parseArgs({
  options: {
    out: { type: "string", default: resolve(root, "out/sd/fall-of-duty.fodp") },
    positions: { type: "string", default: "q16" },
    shots: { type: "string", default: "indexed" },
    storage: { type: "string", default: "deflate" },
    "reference-set": { type: "string" },
    "voice-evidence": { type: "string" },
    "texture-max": { type: "string", default: "128" },
    "walk-frames": { type: "string", default: "6" },
    "run-frames": { type: "string", default: "6" },
  },
});
if (!["q16", "f32"].includes(values.positions)) throw new Error("Use --positions q16 or --positions f32.");
if (!["indexed", "direct"].includes(values.shots)) throw new Error("Use --shots indexed or --shots direct.");
if (!["deflate", "raw"].includes(values.storage)) throw new Error("Use --storage deflate or --storage raw.");
const options = {
  seed: 0x4d7a3eb1,
  maxTexture: Number(values["texture-max"]),
  walkFrames: Number(values["walk-frames"]),
  runFrames: Number(values["run-frames"]),
};
for (const [key, low, high] of [
  ["maxTexture", 32, 1024],
  ["walkFrames", 1, 20],
  ["runFrames", 0, 20],
]) {
  if (!Number.isInteger(options[key]) || options[key] < low || options[key] > high) {
    throw new Error(`${key} must be an integer from ${low} through ${high}.`);
  }
}

class Writer {
  buffer = Buffer.alloc(65536);
  size = 0;

  reserve(length) {
    const offset = this.size;
    this.size += length;
    if (this.size > this.buffer.length) {
      const grown = Buffer.alloc(Math.max(this.buffer.length * 2, this.size));
      this.buffer.copy(grown);
      this.buffer = grown;
    }
    return offset;
  }
  bytes(bytes) {
    const offset = this.reserve(bytes.length);
    bytes.copy(this.buffer, offset);
  }
  u8(value) {
    const offset = this.reserve(1);
    this.buffer.writeUInt8(value, offset);
  }
  u16(value) {
    const offset = this.reserve(2);
    this.buffer.writeUInt16LE(value, offset);
  }
  i16(value) {
    const offset = this.reserve(2);
    this.buffer.writeInt16LE(value, offset);
  }
  u32(value) {
    const offset = this.reserve(4);
    this.buffer.writeUInt32LE(value, offset);
  }
  f32(value) {
    const offset = this.reserve(4);
    this.buffer.writeFloatLE(value, offset);
  }
  floats(values) {
    for (const value of values) this.f32(value);
  }
  finish() {
    return this.buffer.subarray(0, this.size);
  }
}

// IEEE CRC-32. Node 22's built-in implementation lives in zlib. Construct the
// table once so the pack format works without that API across supported tools.
const crcTable = Uint32Array.from({ length: 256 }, (_, byte) => {
  for (let bit = 0; bit < 8; bit++) byte = (byte >>> 1) ^ (byte & 1 ? 0xedb88320 : 0);
  return byte >>> 0;
});
function sectionCrc(bytes) {
  let c = 0xffffffff;
  for (const byte of bytes) c = (c >>> 8) ^ crcTable[(c ^ byte) & 0xff];
  return (c ^ 0xffffffff) >>> 0;
}

function compressSection(bytes) {
  const blockBytes = 8192;
  const writer = new Writer();
  for (let offset = 0; offset < bytes.length; offset += blockBytes) {
    const block = bytes.subarray(offset, offset + blockBytes);
    const compressed = deflateSync(block, { level: 9 });
    const stored = compressed.length < block.length ? compressed : block;
    writer.u16(stored.length);
    writer.bytes(stored);
    if (writer.size >= bytes.length) return bytes;
  }
  return writer.finish();
}

function writeChunks(writer, chunks) {
  for (const entry of chunks) {
    writer.floats(entry.slice(0, 6));
    writer.u32(entry[6]);
    writer.u32(entry[7]);
  }
}

function hasSameTopology(mesh, reference) {
  return (
    mesh?.vertices.length === reference?.vertices.length &&
    mesh?.triangles.length === reference?.triangles.length &&
    mesh.triangles.every((value, i) => value === reference.triangles[i]) &&
    mesh.vertices.every((value, i) => i % 5 < 3 || value === reference.vertices[i])
  );
}

function shotKey(coords) {
  // Use the stored float values and retain the sign bit of IEEE zero.
  return coords.map((coordinate) => (Object.is(coordinate, -0) ? "-0" : coordinate)).join(",");
}

function meshBytes(mesh, writer, positionsOnly, q16, metrics) {
  const { vertices, triangles, chunks } = mesh;
  const min = [Infinity, Infinity, Infinity],
    max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < vertices.length; i += 5) {
    for (let axis = 0; axis < 3; axis++) {
      const value = vertices[i + axis];
      if (!Number.isFinite(value)) throw new Error("Mesh contains a nonfinite vertex.");
      min[axis] = Math.min(min[axis], value);
      max[axis] = Math.max(max[axis], value);
    }
  }
  writer.u32(vertices.length / 5);
  writer.u32(positionsOnly ? 0 : triangles.length / 5);
  writer.u32(positionsOnly ? 0 : chunks.length);
  // Consecutive locomotion poses reference the idle mesh's topology. The
  // resident representation contains vertices only for those poses.
  writer.u32(positionsOnly ? 1 : 0);
  if (q16) {
    writer.floats(min);
    writer.floats(max);
  }
  for (let i = 0; i < vertices.length; i += 5) {
    for (let axis = 0; axis < 3; axis++) {
      const value = vertices[i + axis];
      if (q16) {
        const range = max[axis] - min[axis];
        const code = range > 0 ? Math.round(65535 * ((value - min[axis]) / range)) : 0;
        writer.u16(code);
        const decoded = Math.fround(Math.fround(min[axis]) + (code * (Math.fround(max[axis]) - Math.fround(min[axis]))) / 65535);
        metrics.positionErrorMeters = Math.max(metrics.positionErrorMeters, Math.abs(decoded - Math.fround(value)));
      } else writer.f32(value);
    }
    if (!positionsOnly) writer.floats(vertices.slice(i + 3, i + 5));
  }
  if (!positionsOnly) {
    for (let i = 0; i < triangles.length; i += 5) {
      writer.u32(triangles[i]);
      writer.u32(triangles[i + 1]);
      writer.u32(triangles[i + 2]);
      writer.u16(triangles[i + 3]);
      writer.i16(triangles[i + 4]);
    }
    writeChunks(writer, chunks);
  }
  const vertexCount = vertices.length / 5;
  if (positionsOnly) return vertexCount * 12;
  return vertexCount * 20 + (triangles.length / 5) * 16 + chunks.length * 32;
}

function createPack(assets, sourceRevision, encodings) {
  const q16 = encodings.positions === "q16";
  const indexed = encodings.shots === "indexed";
  const compressed = encodings.storage === "deflate";
  const schema = compressed ? 7 : 6;
  const sections = [];
  const metrics = { positionErrorMeters: 0, singleRivalTopology: true, adsVertexCorrespondence: [] };
  const add = (id, name, encoding, build) => {
    const writer = new Writer();
    const resident = build(writer);
    const raw = writer.finish();
    const bytes = compressed ? compressSection(raw) : raw;
    sections.push({ id, name, encoding, bytes, unpacked: raw.length, resident, crc: sectionCrc(raw) });
  };
  add(1, "materials", 0, (writer) => {
    writer.u32(assets.materials.length);
    for (const mat of assets.materials) {
      writer.u16(mat.color);
      writer.i16(mat.texture);
      writer.u8(mat.alpha);
      writer.u8(mat.doubleSided ? 1 : 0);
    }
    return assets.materials.length * 6;
  });
  add(2, "textures", 0, (writer) => {
    writer.u32(assets.textures.length);
    let size = 0;
    for (const texture of assets.textures) {
      writer.u16(texture.width);
      writer.u16(texture.height);
      const pixels = Buffer.from(texture.pixels, "base64");
      writer.bytes(pixels);
      size += pixels.length;
    }
    return size;
  });
  add(3, "mesh geometry", q16 ? 1 : 0, (writer) => {
    writer.u32(assets.meshes.length);
    let resident = 0;
    for (const [i, mesh] of assets.meshes.entries()) {
      const reference = i > 8 ? assets.meshes[8] : i >= 5 && i <= 7 ? assets.meshes[i - 4] : null;
      const shared = reference !== null && hasSameTopology(mesh, reference);
      if (i >= 5 && i <= 7) metrics.adsVertexCorrespondence.push(shared);
      if (i > 8 && !shared) metrics.singleRivalTopology = false;
      resident += meshBytes(mesh, writer, shared, q16, metrics);
    }
    return resident;
  });
  add(4, "player shot surfaces", indexed ? 1 : 0, (writer) => {
    const { triangles, vertices, chunks } = assets.shot;
    writer.u32(triangles.length / 5);
    writer.u32(chunks.length);
    writer.u32(assets.surfaceNames.length);
    const vertexMap = new Map();
    if (indexed) {
      for (let i = 0; i < triangles.length; i += 5) {
        for (let axis = 0; axis < 3; axis++) {
          const start = triangles[i + axis] * 5;
          const coords = vertices.slice(start, start + 3).map(Math.fround);
          const key = shotKey(coords);
          if (!vertexMap.has(key)) vertexMap.set(key, { index: vertexMap.size, coords });
        }
      }
      writer.u32(vertexMap.size);
      for (const { coords } of vertexMap.values()) writer.floats(coords);
      metrics.uniqueShotVertices = vertexMap.size;
    }
    for (let i = 0; i < triangles.length; i += 5) {
      for (let j = 0; j < 3; j++) {
        const v = triangles[i + j] * 5;
        if (indexed) {
          const index = vertexMap.get(shotKey(vertices.slice(v, v + 3).map(Math.fround))).index;
          if (vertexMap.size <= 65536) writer.u16(index);
          else writer.u32(index);
        } else writer.floats(vertices.slice(v, v + 3));
      }
      writer.u32(triangles[i + 3]);
      writer.i16(triangles[i + 4]);
    }
    writeChunks(writer, chunks);
    const triangleCount = triangles.length / 5;
    const shotVertices = indexed ? vertexMap.size : triangleCount * 3;
    const shotIndices = indexed ? triangleCount * 3 : 0;
    return shotVertices * 12 + shotIndices * 4 + triangleCount * 8 + chunks.length * 32;
  });
  add(5, "movement obstacles", 0, (writer) => {
    writer.u32(assets.obstacles.length);
    for (const obstacle of assets.obstacles) writer.floats(obstacle);
    return assets.obstacles.length * 36;
  });
  add(6, "bot navigation", 0, (writer) => {
    writer.u32(assets.navNodes.length);
    writer.u32(assets.navEdges.length);
    for (const [x, y, z, first, count, cover, walkable] of assets.navNodes) {
      writer.floats([x, y, z]);
      writer.u32(first);
      writer.u32(count);
      writer.u8(cover);
      writer.u8(walkable);
    }
    for (const [to, cost] of assets.navEdges) {
      writer.u32(to);
      writer.f32(cost);
    }
    return assets.navNodes.length * 24 + assets.navEdges.length * 8;
  });
  add(7, "glass pane strength", 0, (writer) => {
    writer.u32(assets.glassHits.length);
    writer.bytes(Buffer.from(assets.glassHits));
    return assets.glassHits.length;
  });
  add(8, "audio", 0, (writer) => {
    writer.u32(assets.audio.length);
    let size = 0;
    for (const clip of assets.audio) {
      const pcm = Buffer.from(clip.samples, "base64");
      writer.u32(pcm.length / 2);
      writer.bytes(pcm);
      size += pcm.length;
    }
    return size;
  });
  add(9, "posed rival hitboxes", 0, (writer) => {
    writer.u32(assets.rivalHitboxes.length);
    let size = 0;
    for (const frame of assets.rivalHitboxes) {
      writer.u32(frame.length);
      for (const hitbox of frame) writer.floats(hitbox);
      size += frame.length * 64;
    }
    return size;
  });
  add(10, "weapon motion tracks", q16 ? 1 : 0, (writer) => {
    writer.u32(assets.tracks.length);
    let resident = 0;
    for (const [id, track] of assets.tracks.entries()) {
      const reference = assets.meshes[[1, 2, 3, 2, 1, 3][id]];
      writer.u32(track.length);
      resident += track.length * 56;
      for (const { seconds, mesh } of track) {
        if (!hasSameTopology(mesh, reference)) throw new Error(`Mismatched weapon topology in track ${id} at ${seconds}s.`);
        writer.f32(seconds);
        for (let i = 0; i < mesh.vertices.length; i += 5) {
          for (let axis = 0; axis < 3; axis++) {
            const value = mesh.vertices[i + axis];
            if (!Number.isFinite(value) || Math.abs(value) >= 3.99) throw new Error("Weapon pose is outside the encoded range.");
            if (q16) {
              const code = Math.round(value * 8192);
              writer.i16(code);
              metrics.positionErrorMeters = Math.max(metrics.positionErrorMeters, Math.abs(value - code / 8192));
            } else writer.f32(value);
          }
        }
        resident += (mesh.vertices.length / 5) * (q16 ? 6 : 12);
      }
    }
    return resident;
  });
  add(11, "strike jet", q16 ? 1 : 0, (writer) => meshBytes(assets.jet, writer, false, q16, metrics));

  add(12, "ADPCM rival voices", 0, (writer) => {
    writer.u32(assets.voices.length);
    let bytes = 0;
    for (const bank of assets.voices) {
      writer.u32(bank.clips.length);
      for (const { frames, predictor, index, codes } of bank.clips) {
        writer.u32(frames);
        writer.i16(predictor);
        writer.u8(index);
        writer.u8(0);
        writer.bytes(codes);
        bytes += codes.length;
      }
    }
    return bytes;
  });
  add(13, "sky cubemap", 0, (writer) => {
    let bytes = 0;
    for (const face of assets.sky) {
      const pixels = Buffer.from(face, "base64");
      writer.bytes(pixels);
      bytes += pixels.length;
    }
    return bytes;
  });
  add(14, "Hanz and player meshes", q16 ? 1 : 0, (writer) => {
    writer.u32(assets.hanzFrames.length);
    let resident = 0;
    for (const [i, mesh] of assets.hanzFrames.entries()) {
      if (i && !hasSameTopology(mesh, assets.hanzFrames[0])) throw new Error("Hanz mesh topology changes across frames.");
      resident += meshBytes(mesh, writer, i !== 0, q16, metrics);
    }
    return resident + meshBytes(assets.playerBody, writer, false, q16, metrics);
  });

  const header = new Writer();
  header.bytes(Buffer.from("FODPACK\0"));
  header.u32(schema);
  header.u32(sections.length);
  let offset = 48 + sections.length * (compressed ? 28 : 24);
  header.u32(offset + sections.reduce((size, { bytes }) => size + bytes.length, 0));
  header.u32(options.seed);
  header.bytes(Buffer.from(sourceRevision, "hex"));
  header.u16(options.walkFrames);
  header.u16(options.runFrames);
  for (const section of sections) {
    header.u32(section.id);
    header.u32(section.encoding);
    header.u32(offset);
    header.u32(section.bytes.length);
    header.u32(section.resident);
    header.u32(section.crc);
    if (compressed) header.u32(section.unpacked);
    offset += section.bytes.length;
  }

  return {
    schema,
    pack: Buffer.concat([header.finish(), ...sections.map((section) => section.bytes)]),
    sections: sections.map(({ name, bytes, unpacked, resident, crc }) => ({
      name,
      storedBytes: bytes.length,
      unpackedBytes: unpacked,
      decodedPayloadBytes: resident,
      crc32: crc.toString(16).padStart(8, "0"),
    })),
    metrics,
  };
}

async function capture() {
  const cacheDir = await mkdtemp(resolve(tmpdir(), "fod-assets-"));
  const server = await createServer({
    configFile: false,
    root,
    cacheDir,
    server: { host: "127.0.0.1", port: 0, watch: null },
    plugins: [
      {
        name: "fod-offline-assets",
        configureServer(instance) {
          instance.middlewares.use("/__fod_export", (_req, res) => {
            res.setHeader("Content-Type", "text/html");
            res.end("<!doctype html><title>Fall of Duty asset export</title>");
          });
        },
      },
    ],
  });
  let browser;
  try {
    await server.listen();
    const address = server.httpServer.address();
    browser = await chromium.launch({ headless: true, args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"] });
    const page = await browser.newPage({ viewport: { width: 800, height: 450 }, deviceScaleFactor: 1 });
    page.on("console", (msg) => {
      if (["error", "warning"].includes(msg.type()) || !msg.text().startsWith("[vite]")) console.error(msg.text());
    });
    await page.goto(`http://127.0.0.1:${address.port}/__fod_export`);
    const oldCharacterRequests = [];
    page.on("request", (request) => {
      if (new URL(request.url()).pathname.endsWith("/models/soldier.glb.gz")) oldCharacterRequests.push(request.url());
    });
    const captured = await page.evaluate(async (args) => {
      const { captureAssets } = await import("/tools/browser-assets.mjs");
      return await captureAssets(args);
    }, options);
    if (oldCharacterRequests.length) throw new Error("Lite must build without requesting the browser character model.");
    return captured;
  } finally {
    await browser?.close();
    await server.close();
    await rm(cacheDir, { recursive: true, force: true });
  }
}

const sourceRevision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: root, encoding: "utf8" }).trim();
const sourceDirty = !!execFileSync("git", ["status", "--porcelain", "--", "reference/browser", "tools", "package-lock.json"], {
  cwd: root,
  encoding: "utf8",
}).trim();
const lock = JSON.parse(await readFile(resolve(root, "package-lock.json"), "utf8"));
const assets = await capture();
assets.voices = assets.voices.map((bank) => ({
  ...bank,
  clips: bank.clips.map(({ name, samples }) => ({
    name,
    ...encodeVoice(Buffer.from(samples, "base64")),
    evidencePcm: values["voice-evidence"] && name.endsWith("match-start-01.mp3") ? Buffer.from(samples, "base64") : null,
  })),
}));
const voiceMetrics = assets.voices.flatMap((bank) =>
  bank.clips.map(({ name, frames, codes, signal, error }) => ({
    name,
    sampleCount: frames,
    pcmBytes: frames * 2,
    adpcmBytes: codes.length + 8,
    snrDb: error ? Number((10 * Math.log10(signal / error)).toFixed(2)) : null,
  }))
);
if (values["voice-evidence"]) {
  const folder = resolve(values["voice-evidence"]);
  await mkdir(folder, { recursive: true });
  const example = assets.voices.flatMap((bank) => bank.clips).find((clip) => clip.evidencePcm);
  await writeFile(resolve(folder, "hanz-pcm.wav"), wav(example.evidencePcm));
  await writeFile(resolve(folder, "hanz-adpcm.wav"), wav(example.reconstructed));
  await writeFile(resolve(folder, "voice-metrics.json"), JSON.stringify(voiceMetrics, null, 2) + "\n");
}
const manifestBase = {
  format: "FODPACK",
  sourceRevision,
  sourceDirty,
  inputLockSha256: createHash("sha256")
    .update(await readFile(resolve(root, "package-lock.json")))
    .digest("hex"),
  babylon: lock.packages["node_modules/@babylonjs/core"].version,
  playwright: lock.packages["node_modules/playwright"].version,
  art: options,
  characters: ["IPwndUrMom", "Hanz", "Player"],
  textureSizes: assets.textures.map(({ name, width, height }) => [name, width, height]),
  meshCounts: assets.meshes.map(({ vertices, triangles, chunks }) => ({
    vertices: vertices.length / 5,
    triangles: triangles.length / 5,
    chunks: chunks.length,
  })),
  weaponTracks: assets.tracks.map((track) => track.map(({ seconds }) => seconds)),
  boltClearance: assets.boltClearance,
  hanzPoseTriangles: assets.hanzFrames[0].triangles.length / 5,
  playerTriangles: assets.playerBody.triangles.length / 5,
  jetTriangles: assets.jet.triangles.length / 5,
  shotSurfaces: assets.surfaceNames,
  shotTriangleCount: assets.shot.triangles.length / 5,
  obstacles: assets.obstacles.length,
  navNodes: assets.navNodes.length,
  navEdges: assets.navEdges.length,
  hitboxFrames: assets.rivalHitboxes.length,
  glass: assets.glassHits,
  voiceCodec: "IMA ADPCM, mono 24 kHz",
  voices: voiceMetrics,
  audio: assets.audio.map(({ name, samples }) => [name, Buffer.from(samples, "base64").length / 2]),
};

async function savePack(output, encodings) {
  const { schema, pack, sections, metrics } = createPack(assets, sourceRevision, encodings);
  const manifest = {
    ...manifestBase,
    schema,
    encodings,
    ...metrics,
    storedBytes: pack.length,
    sections,
    sha256: createHash("sha256").update(pack).digest("hex"),
  };
  await mkdir(dirname(output), { recursive: true });
  for (const [path, bytes] of [
    [output, pack],
    [`${output}.json`, `${JSON.stringify(manifest, null, 2)}\n`],
  ]) {
    const temporary = `${path}.tmp-${process.pid}`;
    await writeFile(temporary, bytes);
    await rename(temporary, path);
  }
  console.log(`Exported ${output} (${pack.length} bytes), SHA-256 ${manifest.sha256}`);
}

await savePack(resolve(values.out), { positions: values.positions, shots: values.shots, storage: values.storage });
if (values["reference-set"]) {
  const folder = resolve(values["reference-set"]);
  await savePack(resolve(folder, "full.fodp"), { positions: "f32", shots: "direct", storage: "raw" });
  await savePack(resolve(folder, "q16-only.fodp"), { positions: "q16", shots: "direct", storage: "raw" });
  await savePack(resolve(folder, "indexed-only.fodp"), { positions: "f32", shots: "indexed", storage: "raw" });
}
