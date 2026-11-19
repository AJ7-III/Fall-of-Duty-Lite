# Fall of Duty Lite

A compact C++ first-person shooter for the Guition JC4880P443C_I_W ESP32-P4 handheld and SDL2 desktops. Play an offline first-to-ten duel on Ship Box with three weapons, ten difficulty levels, two rivals, and 27 voice lines. The game renders at 400×225 or 320×180. The included asset pack occupies 3.65 MB.

[Download firmware and SD files](https://github.com/AJ7-III/Fall-of-Duty-Lite/releases/latest) · [Handheld installation](INSTALL.md) · [Board build and qualification](BOARD.md) · [Replay and performance results](EVIDENCE.md)

The handheld package is cross-built for pre-v3 ESP32-P4 boards. Frame delivery, latency, and full-duel play on the Guition still need hardware qualification.

## Build and run on desktop

From the repository root, use CMake 3.22+, Ninja, a C++17 compiler, zlib 1.2.9+, and SDL2 development files. The checked-in asset pack is ready to use. On macOS, the Command Line Tools SDK supplies zlib, and Homebrew can install CMake, Ninja, and SDL2.

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/host
ctest --test-dir build/host --output-on-failure
build/host/fod_host
```

Use Enter to start, WASD to move, mouse to look, left mouse to fire, and right mouse or Space to aim. Shift sprints, Z jumps, C crouches or goes prone, R reloads, X swaps weapons, and 4 deploys a streak. Esc pauses. A supported SDL gamepad uses the same controls as the handheld. Use `-DFOD_DESKTOP=OFF` for an SDL-free command-line build and tests.

## Replay and benchmark

The repository includes eight reference fixtures. The following commands use the included game pack and run from the repository root.

```sh
build/host/fod_host --bench reference/fixtures/long-lane.fodtrace --mode both
build/host/fod_host --bench reference/fixtures/apache.fodtrace --mode both
python3 tools/run-evidence.py \
  --host build/host/fod_host --pack assets/fall-of-duty.fodp \
  --reference reference/fixtures --out out/evidence
```

Use `--trace PATH --journal PATH` for a single replay. Add `--image PATH --image-tick N` for a headless view at a specific tick. The desktop renderer also supports `--look classic` for comparisons with the earlier visual path.

## Regenerate game data

Only the optional export tools require Node.js and Chromium. Use Node 20.19+, 22.13+, or 24+ within the supported release lines in `package.json`.

```sh
npm ci
npx playwright install chromium
npm run data:check
npm run assets:export
```

The exporter writes `out/sd/fall-of-duty.fodp`. Pass that path to the desktop executable with `--pack` to try the new export. The [asset pipeline](ASSET_PIPELINE.md) explains the format, provenance, and optional browser capture.

## Package the handheld build

Follow the [ESP-IDF v5.5.3 build instructions](BOARD.md) to build `firmware/build-lite` and activate the pinned SDK environment. Then run from the repository root.

```sh
python3 tools/package-release.py \
  --firmware firmware/build-lite --idf "$IDF_PATH" \
  --sd assets --reference reference/fixtures \
  --out out/Fall-of-Duty-Lite-Guition-ESP32-P4.zip
```

The package includes the bootloader, partition table, application, SD files, replay traces, and SHA-256 checksums. [See the installation guide for board setup and controls.](INSTALL.md)

The browser project is [Fall of Duty](https://github.com/AJ7-III/Fall-of-Duty). The [reference snapshot](reference/browser/README.md) supplies the asset exporter and gameplay data generator. See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md) for attribution.
