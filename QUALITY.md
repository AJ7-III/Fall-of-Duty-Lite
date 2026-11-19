# Audio and rendering

Lite includes all 27 rival voice lines. The mixer decodes mono 24 kHz IMA ADPCM on demand and keeps 1,025,648 bytes of voice codes in memory. Voice playback shuffles each event group, avoids repeats across shuffles, and displays the current speaker under the radar. Pause holds the current line and the Rival Voice setting mutes speech. Effects and elimination chimes use PCM.

The enhanced renderer uses texture mipmaps, 16-bit reciprocal-depth encoding, and a dithered sky cubemap. It preserves the same 800×450 logical viewport at 400×225 and 320×180. Large foreground artwork includes a 1024×512 graffiti atlas. The original Lite characters and arms use their own geometry, and the weapon motion tracks follow the browser reference.

The desktop `--look classic` option switches to the earlier rendering path. For timings and deterministic playback, see [replay and performance evidence](EVIDENCE.md).
