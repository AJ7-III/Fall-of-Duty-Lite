# Replay and performance evidence

## Runtime geometry layout

The optimized engine retains shared full-precision collision vertices and stores animation positions separately from the base mesh's UVs and topology. The first-person weapon pass uses vertices already in camera space. It skips the weapon pass and its depth clear when the scope hides the model.

With the same schema 7 SD pack, desktop loader accounting measured 9,553,337 resident asset bytes, down from 10,859,021. Estimated peak asset-loading memory fell from 10,940,941 to 9,635,257 bytes. The original paired firmware cross-build measured 851,552 bytes. Both render looks matched the previous renderer across eight replay scenarios at both resolutions, with 48 identical checkpoint images and 32 identical gameplay journals. The Release and sanitizer test suites passed. [Runtime measurements](reference/measurements/runtime-benchmark.json)

Across three paired desktop trials per scene and resolution, median p95 frame time was between 2.3% faster and 0.1% slower. The run-to-run spread makes the frame-time change inconclusive.

## Projection reuse

The earlier renderer pass reuses 128 vertex projections across adjacent triangles and evaluates dynamic face lighting after screen-space culling. It added 3 KiB of scratch storage. Geometry setup p95 fell 29–33% in desktop tests and whole-frame p95 fell 1.6–14.6%. [Projection benchmark](reference/measurements/projection-benchmark.json)

## Compact asset pack

The original schema 7 pack occupied 3,653,826 bytes. Lossless section compression reduced the 8,394,137-byte schema 6 file by 56.5%. All 14 payloads matched byte for byte in that comparison. The P4 decoder adds about 19 KiB of temporary storage during asset loading. Median desktop load time rose from 23.0 to 37.9 ms in the original storage comparison. [Section-by-section results](reference/measurements/storage-comparison.json)

The weapon-art pack occupied 3,917,627 bytes. The M40A3 uses 15 sampled poses for its bolt cycle and 29 for the two-round reload, with a matching pose at both ends of the repeated feed. The MP44, M40A3 and USP45 each use fitted firing-hand joints in their existing mesh and animation tracks. The loader reported 9,826,953 resident asset bytes and 9,908,873 estimated peak asset-loading bytes for that pack. The world meshes, shot geometry, movement obstacles, navigation map, hitboxes, and audio matched the earlier pack byte for byte. All eight replay journals matched at both render sizes.

Across three paired desktop weapon-cycle trials for the M40A3 animation update, median frame work changed from 1.029 to 1.009 ms at 400×225 and from 0.740 to 0.734 ms at 320×180. The corresponding p95 times changed from 1.246 to 1.255 ms and from 0.920 to 0.936 ms. The Guition firmware cross-build passed.

## Texture sampling and edge smoothing

The renderer uses a 640- or 800-byte row buffer for diagonal edge smoothing. In three paired desktop trials per scene, the new renderer and pack added 0.00–0.14 ms to median p95 frame work over the brush build across the Apache and long-lane traces at 320×180 and 400×225. These timings exclude live scanout. The ESP-IDF cross-build produced an 854,240-byte app image, and the verified local firmware and SD archive occupies 4,245,666 bytes.

## Baked daylight

The daylight comparison used the same 3,723,292-byte pack in both runs. Renderer working memory rose from 1,074,242 to 1,281,456 bytes at 400×225. The increase holds the ground light map and four bytes of light for each world triangle. Constructing the renderer takes 35.7 ms on the desktop, up from 1.2 ms.

Across five paired desktop trials per scene and resolution, median frame work rose 8.6–10.5% and p95 frame work rose 1.8–9.7% on the Apache and long-lane traces. Median geometry setup rose 6.6–12.3% and median raster time rose 8.9–12.6%. These timings exclude live scanout. The handheld cost of the bake and of the added frame work is unmeasured.

The ESP-IDF cross-build produced an 878,912-byte app image, up from 854,240, and the local firmware and SD archive occupies 4,263,727 bytes. Native replay journals still match across both render sizes for all eight fixtures. Fourteen classic-look viewpoints rendered byte-identical images before and after the change.

The Release and sanitizer test suites pass. All eight replays rendered 200 frames at each size under the address and undefined-behavior sanitizers without a report. An earlier sanitizer replay reported an out-of-range integer conversion where a nearly level triangle edge crosses a row far off screen. That code predates the daylight work, and the renderer now bounds the crossing before converting it.

## Replay fixtures

The [checked-in browser traces](reference/fixtures) cover long-lane combat, glass breakage, movement, weapon cycles, the airstrike laptop, the Apache fly-in, death timing, and match point across 3,076 steps. The accompanying browser journals record gameplay random draws, positions, ammo, shots, and scores. Native replay journals match across both render sizes. The strict browser comparison allows only the recorded Apache height difference for the dead rival at ticks 564–570, bounded to 7 cm with horizontal position and the rest of the state still checked.

The 360-tick `ffa-apache` native trace checks both live rivals and the incoming Apache in one rendered frame at each resolution. It uses a fixed seed and records both rival states and the attacker and rival slots of gameplay events. The pack includes a bidirectional route out of the isolated south-edge pocket. The host test drives a rival out of that pocket, and the two rivals share A* search buffers while retaining separate paths. [Pack manifest](assets/fall-of-duty.fodp.json)

Journal-only playback skips renderer initialization and framebuffer allocation. Across five paired desktop runs of all 12 fixtures, median wall time for replay verification fell from 1.83 to 1.12 seconds. All 24 gameplay journals matched byte for byte.

The native tests complete first-to-ten duels against both rivals. The firmware and SD release have been cross-built and checked for flash layout and image checksums. The [Guition qualification procedure](BOARD.md#replay-and-measurement) covers the remaining on-device tests for frame delivery, audio, input, and latency.

## Briar House campaign

There are three campaign traces:

- **Breakout (737 ticks).** Opens fire from overwatch and records a plasma shock and the arrival of flankers.
- **Landing zone (842 ticks).** Resumes from the extraction checkpoint and records the timed hold, plasma shock and the Apache fly-in.
- **Finale (1,320 ticks).** Starts The Array at the rear door. It holds Signal to steer the mothership's beam while riflemen push the house, and records the dish link, plasma hits and a shock.

Each trace produces an identical gameplay journal at 320×180 and 400×225. The journal now also records the operation stage, the saddle lock, the dish link and the beam's distance from the dish. [Campaign fixtures](reference/fixtures)

The platform tests also finish a fresh Briar House and a checkpoint-retry run, covering skipped and natural lifts and both Walt escort outcomes. Rule tests on the mission controller cover:

- **Briar House.** Ben waved over from 12–16 m, the dish pulse dropping the Enforcer's linked shield, a Dustoff signal under watch drawing fire, Nora refusing the bird, a civilian lost after rescue, and an obstructed boarding path.
- **Timberline.** Played without Nora: emitter refusals, calls landing unaware, the jammer hijack, the heavy call creeping toward the kit, and a retry that keeps the calls already answered.
- **The Array.** Drifting onto the dish ends the mission, a burned clearing changes the ending, the last jammer holds the lock until shot, and Nora comes when signalled. A second run plays The Array without Nora and ends on Delta-1's line.

The desktop platform test launches **Campaign** from the title screen and drives all three missions at difficulty 5 using scripted movement, aim, fire, Signal and menu input. The scripted player aims instantly, so its campaign clock of **3:25** is far below a human run. The test checks:

- **Briar House.** Ben comes across on a Signal from range.
- **Timberline.** Opens with Nora in the room under the dish. Painted ground moves the saucer, drops land unaware, and four calls complete it.
- **The Array.** Opens with the garden squad unaware and ends with the beam steered around the clearing, a last jammer, the fleet setting down, and the signal to Nora. [Platform test](host/platform_tests.cpp)

On desktop, a batch of 76,670 nearest-node queries across the 3,335-node valley graph matched the original linear search exactly. The median of five runs averaged 0.40 µs per lookup with the column search and 1.62 µs with the full scan. The search uses the existing node order and allocates no additional heap storage. [Navigation code](game_bot.hpp)

## Low-resolution art and motion

Three paired desktop trials per scene and resolution covered Long Lane, Free-for-all with Apache, the Briar House breakout and landing zone, a complete scripted Timberline run, and The Array finale. At 320×180 and 400×225, the median whole-frame p95 changed by −0.022 to +0.058 ms, or −0.9% to +3.2%. These timings use the enhanced renderer and exclude handheld scanout.

The revised SD pack occupies 4,001,984 bytes. Texture dimensions, mesh counts, and decoded section lengths matched the prior pack. Gameplay journals for all 12 checked-in traces and the scripted Timberline run matched byte for byte at both resolutions. [Pack manifest](assets/fall-of-duty.fodp.json)

Three paired desktop runs of the distance-driven sprint cycle and Apache banking covered a scripted sprint with a turn and jump, plus the Free-for-all Apache replay. Across both scenes at 320×180 and 400×225, median whole-frame p95 changed by −0.073 to +0.048 ms. The updated code matched all 28 gameplay journals byte for byte, including the sprint and Timberline runs.
