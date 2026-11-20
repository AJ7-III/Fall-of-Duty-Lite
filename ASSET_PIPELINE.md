# Ship Box asset pipeline

`assets/fall-of-duty.fodp` is the verified, playable pack. The adjacent manifest records its original source revision, art seed, toolchain versions, SHA-256, section checksums, model counts, and surface names. Native builds and replays use it directly.

From the repository root, install the locked dependencies and Playwright Chromium to regenerate assets from the included browser reference.

```sh
npm ci
npx playwright install chromium
npm run data:check
npm run assets:export -- --reference-set out/candidates
```

The exporter runs the browser asset builders in local Chromium. It writes to `out/sd/` by default. `--storage raw` writes a schema 6 pack. Use that flag with `--reference-set` to measure geometry encodings with raw storage across all candidates. Synthesized effects can vary across exports. The checked-in pack retains the original release audio payload.

## Texture finishing

The exporter grades and sharpens surfaces at their final texture dimensions before converting them to RGB565. The [texture finishing pass](tools/texture-treatment.mjs) uses separate profiles for printed art, painted metal, paving, soil and weapon materials. It limits sharpening and adds seamless patina to surfaces such as corrugated steel and concrete.

The cloud tones are sampled in panorama space before export to six 64×64 luminance faces. The renderer interpolates cloud tones and looks up the sky color in a small RGB565 palette. The game draws the HUD at display resolution.

The exporter samples one in three of the browser yard's grass placements and builds three opaque blades for each tuft. The blades share three flat colors and use the same spatial chunks as the rest of the yard. The ground uses the existing 128×128 grass tile in a greener palette. Foliage is included only in the visual mesh.

## Infantry and weapon finishes

The native infantry builder uses a contoured head mesh and separate 64×64 face textures for all three characters. Longer legs and torsos raise their silhouettes by 5 cm with the soles at the original height. Padded fingers, wide cuffs, and fuller sleeves give the first-person grips more volume. The closer wrist position blends out during weapon motion. RGB565 textures distinguish woven sleeves, stippled gloves, walnut and blued steel. The M40A3 has a three-tone painted stock and a tinted highlight on the lens.

Briar House builds its masonry, shingles, floors, cabin siding and glass locally from procedural tiles. Its campaign renderer uses the shared cloud data with a separate dusk palette.

## Pack layout

The little-endian FODPACK header carries schema 7, the file size, source revision, art seed, and locomotion frame counts. Each 28-byte section entry holds the ID, asset encoding, offset, stored length, resident size estimate, CRC-32, and unpacked length. The CRC covers the unpacked bytes before asset decoding.

Equal stored and unpacked lengths identify a raw section. A shorter section holds independent blocks of up to 8 KiB unpacked. Each block starts with a 16-bit stored length. Equal block lengths identify raw bytes. A shorter block is a zlib stream. The firmware uses the ESP32-P4 ROM inflater and frees its scratch buffers after loading. The reader validates section sizes and checksums, enforces a 12 MiB asset loading allocation limit, and also accepts schemas 2–6.

The q16 visual encoding reconstructs positions from mesh bounds during loading. Position-only animation poses share UVs and triangles with their base mesh. Player shots use independent full-precision geometry. The indexed encoding retains shared vertices during play, and both shot encodings supply the same coordinates and material IDs to the ray test.

Each first-person weapon fits the firing glove to its grip with a wrist position and individual finger and thumb joints. The M40A3 blends from that grip to the bolt handle as the cycle begins.

The M40A3 tracks sample the browser rig at each bolt and reload phase boundary and between phases. The two-round reload repeats its second feed for additional rounds. Matching press poses let the feed repeat and transition to bolt closure without a visual jump.

The pack holds RGB565 textures, a luminance sky cubemap, original character meshes, three weapon viewmodels with motion tracks, and 27 rival voice lines encoded as mono 24 kHz IMA ADPCM. The mix keeps the voice bank compressed and decodes the active line during playback. [Storage measurements](reference/measurements/storage-comparison.json)

## Campaign dialogue

`campaign_dialogue.inc` defines the stable line IDs and subtitles in export order. The ignored `.voice-sources/raw/` folder holds the original takes, and `.voice-sources/ingest.json` maps each voiced ID to its original filename and SHA-256. After verifying any replacement take against the subtitle, update that entry and rebuild with Node and FFmpeg.

```sh
node tools/build-campaign-voices.mjs
```

The builder writes the mono IMA ADPCM bank, its manifest, and the 24 kHz frame counts used for deterministic subtitle timing. Radio takes are encoded at 12 kHz and direct speech at 24 kHz. The opener uses direct speech for Delta-1, who is in the room with the player. Build the application again after rebuilding the bank. Keep the sidecar beside the base pack on desktop and at the microSD root on handheld.

## Capture a new browser baseline

The included [fixtures](reference/fixtures) support native replay without browser assets. To recapture an upstream visual baseline, place the original reference `soldier.glb.gz` in `reference/browser/public/models/` under its existing usage terms. This path is Git-ignored. The model is used only by the optional browser capture. Lite's asset export builds its own character meshes.

```sh
npm run replays:capture -- --scene long-lane
```

Output goes to `out/reference/`. Omit `--scene` to capture all eight scenarios. For the source revision and attribution, see the [reference snapshot](reference/browser/README.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
