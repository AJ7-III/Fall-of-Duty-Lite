# Audio and rendering

Lite includes all 27 rival voice lines. The mixer decodes mono 24 kHz IMA ADPCM on demand and keeps 1,025,648 bytes of voice codes in memory. Both rivals share one voice channel. A line plays to completion, followed by a randomized pause of 0.75 to 1.5 seconds. Taunts that arrive during speech or the pause expire. Each taunt has an independent chance to play, and each event group uses a shuffled deck that avoids repeats across shuffles. The current speaker appears under the radar. Pause holds the current line and the Rival Voice setting mutes speech. Effects and elimination chimes use PCM.

The enhanced renderer uses texture mipmaps, 16-bit reciprocal-depth encoding, and a dithered sky cubemap. It preserves the same 800×450 logical viewport at 400×225 and 320×180. Ship Box uses mild texture sharpening on the steel, paving, and 1024×512 graffiti atlas.

The renderer blends neighboring texture detail levels based on how much surface each pixel covers. On large receding faces, it updates that estimate along short spans to keep the near end sharp and avoid harsh transitions in the distance. A row-buffer pass softens high-contrast diagonal steps in the world and viewmodel. The HUD is drawn afterward at display resolution.

## Daylight

The enhanced renderer lights Ship Box with overcast daylight that it works out while the pack loads. It records the height span of the geometry in each 16 cm column of the yard, then traces a pale sun and the open sky across those columns. A roof leaves the air below it open, so the walk-through containers stay dim inside. Turf, kerbs, and floor slabs cast no shade.

The bake produces a 256×256 ground light map with four coarser levels and a light level for each corner of each large world face. Small faces take one level. Ground faces read the map at the ends of 16-pixel spans and pick the level that matches the ground under one pixel. Other faces interpolate their corners with a two-pixel ordered dither, which keeps the gradient from banding in five-bit color.

The rival and the first-person weapon dim with the ground they stand on, and the rival casts a soft contact shadow. Geometry beyond 18 m fades toward the horizon tone of the pack's sky. Rain falls in two layers, with short faint streaks behind longer ones.

The desktop scales the enhanced picture to 800×480 with bilinear filtering, which is how the handheld's PPA scales it. Captures written with `--image` use the same filter.

Ship Box's steel, paving, grass, and gear use baked pigment and surface wear at their original texture sizes. Briar House's procedural materials add distinct dry and damp surfaces. Moving meshes take their fill and direct light from the arena's lighting profile, and the static bake varies the fill beneath shadows with the distance to the blocker.

The weapon pumps through an eight-key sprint cycle driven by distance along the ground. It follows quick turns with a brief delay and dips on landing. The sights settle onto the existing aim track as the player raises them. Rivals, allies, civilians, and aliens lean into travel through their existing mesh transforms. Infantry walk and run poses ease out of planted steps and roll the toe forward as the foot swings. The Apache pitches into forward flight and banks as it turns, using the same attitude as its gun mount.

The desktop `--look classic` option switches to the earlier rendering path, with flat lighting and nearest-neighbor scaling. For timings and deterministic playback, see [replay and performance evidence](EVIDENCE.md).
