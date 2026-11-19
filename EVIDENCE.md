# Replay and performance evidence

## Runtime geometry layout

The optimized engine retains shared full-precision collision vertices and stores animation positions separately from the base mesh's UVs and topology. The first-person weapon pass uses vertices already in camera space. It skips the weapon pass and its depth clear when the scope hides the model.

With the same schema 7 SD pack, desktop loader accounting measured 9,553,337 resident asset bytes, down from 10,859,021. Estimated peak asset-loading memory fell from 10,940,941 to 9,635,257 bytes. The original paired firmware cross-build measured 851,552 bytes. Both render looks matched the previous renderer across eight replay scenarios at both resolutions, with 48 identical checkpoint images and 32 identical gameplay journals. The Release and sanitizer test suites passed. [Runtime measurements](reference/measurements/runtime-benchmark.json)

Across three paired desktop trials per scene and resolution, median p95 frame time was between 2.3% faster and 0.1% slower. The run-to-run spread makes the frame-time change inconclusive.

## Projection reuse

The earlier renderer pass reuses 128 vertex projections across adjacent triangles and evaluates dynamic face lighting after screen-space culling. It added 3 KiB of scratch storage. Geometry setup p95 fell 29–33% in desktop tests and whole-frame p95 fell 1.6–14.6%. [Projection benchmark](reference/measurements/projection-benchmark.json)

## Compact asset pack

The original schema 7 pack occupied 3,653,826 bytes. Lossless section compression reduced the 8,394,137-byte schema 6 file by 56.5%. All 14 payloads matched byte for byte in that comparison. The P4 decoder adds about 19 KiB of temporary storage during asset loading. Median desktop load time rose from 23.0 to 37.9 ms in the original storage comparison. [Section-by-section results](reference/measurements/storage-comparison.json)

The current pack occupies 3,723,292 bytes, including greener turf and 300 grass tufts. The loader reports 9,621,947 resident asset bytes and 9,703,867 estimated peak asset-loading bytes. The shot geometry, movement obstacles, and navigation map match the original pack byte for byte. [Current pack manifest](assets/fall-of-duty.fodp.json)

## Texture sampling and edge smoothing

The renderer uses a 640- or 800-byte row buffer for diagonal edge smoothing. In three paired desktop trials per scene, the new renderer and pack added 0.00–0.14 ms to median p95 frame work over the brush build across the Apache and long-lane traces at 320×180 and 400×225. These timings exclude live scanout. The ESP-IDF cross-build produced an 854,240-byte app image, and the verified local firmware and SD archive occupies 4,245,666 bytes.

## Replay fixtures

The [checked-in traces](reference/fixtures) cover long-lane combat, glass breakage, movement, weapon cycles, the airstrike laptop, the Apache fly-in, death timing, and match point across 3,076 steps. The accompanying browser journals record gameplay random draws, positions, ammo, shots, and scores. Native replay journals match across both render sizes. The known Apache browser discrepancy at tick 564 concerns the vertical root position of the dead rival. The browser uses the model's foot placement, and Lite renders a sampled body pose.

The native tests complete first-to-ten duels against both rivals. The firmware and SD release have been cross-built and checked for flash layout and image checksums. The [Guition qualification procedure](BOARD.md#replay-and-measurement) covers the remaining on-device tests for frame delivery, audio, input, and latency.
