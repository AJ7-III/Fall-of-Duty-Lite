# Audio and rendering

Lite includes all 27 rival voice lines. The mixer decodes mono 24 kHz IMA ADPCM on demand and keeps 1,025,648 bytes of voice codes in memory. Voice playback shuffles each event group, avoids repeats across shuffles, and displays the current speaker under the radar. Pause holds the current line and the Rival Voice setting mutes speech. Effects and elimination chimes use PCM.

The enhanced renderer uses texture mipmaps, 16-bit reciprocal-depth encoding, and a dithered sky cubemap. It preserves the same 800×450 logical viewport at 400×225 and 320×180. Ship Box uses mild texture sharpening on the steel, paving, and 1024×512 graffiti atlas.

The renderer blends neighboring texture detail levels based on how much surface each pixel covers. On large receding faces, it updates that estimate along short spans to keep the near end sharp and avoid harsh transitions in the distance. A row-buffer pass softens high-contrast diagonal steps in the world and viewmodel. The HUD is drawn afterward at display resolution.

The desktop `--look classic` option switches to the earlier rendering path. For timings and deterministic playback, see [replay and performance evidence](EVIDENCE.md).
