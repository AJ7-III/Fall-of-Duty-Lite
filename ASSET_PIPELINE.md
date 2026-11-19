# Ship Box asset pipeline

`assets/fall-of-duty.fodp` is the verified, playable pack. The adjacent manifest records its original source revision, art seed, toolchain versions, SHA-256, section checksums, model counts, and surface names. Native builds and replays use it directly.

From the repository root, install the locked dependencies and Playwright Chromium to regenerate assets from the included browser reference.

```sh
npm ci
npx playwright install chromium
npm run data:check
npm run assets:export -- --reference-set out/candidates
```

The exporter runs the browser asset builders in local Chromium. It writes to `out/sd/` by default. `--storage raw` writes a schema 6 pack. Use that flag with `--reference-set` to measure geometry encodings with raw storage across all candidates. Synthesized effects can vary across exports. The checked-in schema 7 pack was converted losslessly from the verified release and preserves every asset payload.

## Pack layout

The little-endian FODPACK header carries schema 7, the file size, source revision, art seed, and locomotion frame counts. Each 28-byte section entry holds the ID, asset encoding, offset, stored length, resident size estimate, CRC-32, and unpacked length. The CRC covers the unpacked bytes before asset decoding.

Equal stored and unpacked lengths identify a raw section. A shorter section holds independent blocks of up to 8 KiB unpacked. Each block starts with a 16-bit stored length. Equal block lengths identify raw bytes. A shorter block is a zlib stream. The firmware uses the ESP32-P4 ROM inflater and frees its scratch buffers after loading. The reader validates section sizes and checksums, enforces a 12 MiB asset loading allocation limit, and also accepts schemas 2–6.

The q16 visual encoding reconstructs positions from mesh bounds during loading. Position-only animation poses share UVs and triangles with their base mesh. Player shots use independent full-precision geometry. The indexed encoding retains shared vertices during play, and both shot encodings supply the same coordinates and material IDs to the ray test.

The pack holds RGB565 textures, a luminance sky cubemap, original character meshes, three weapon viewmodels with motion tracks, and 27 rival voice lines encoded as mono 24 kHz IMA ADPCM. The mix keeps the voice bank compressed and decodes the active line during playback. [Storage measurements](reference/measurements/storage-comparison.json)

## Capture a new browser baseline

The included [fixtures](reference/fixtures) support native replay without browser assets. To recapture an upstream visual baseline, place the original reference `soldier.glb.gz` in `reference/browser/public/models/` under its existing usage terms. This path is Git-ignored. The model is used only by the optional browser capture. Lite's asset export builds its own character meshes.

```sh
npm run replays:capture -- --scene long-lane
```

Output goes to `out/reference/`. Omit `--scene` to capture all eight scenarios. For the source revision and attribution, see the [reference snapshot](reference/browser/README.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
