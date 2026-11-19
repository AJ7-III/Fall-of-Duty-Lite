# Third-party notices

Fall of Duty Lite includes native code and a build-time reference derived from [Fall of Duty](https://github.com/AJ7-III/Fall-of-Duty). Project copyright and the MIT license are in [LICENSE](LICENSE).

## Browser reference model

The original browser game uses the Vanguard character and animations from [Adobe Mixamo](https://www.mixamo.com/). The raw GLB, its embedded textures, and its animation clips are excluded from this repository and the MIT license. Optional browser-baseline tooling can use a separately supplied copy under [Adobe's terms](https://www.adobe.com/legal/terms.html) and [Mixamo guidance](https://helpx.adobe.com/creative-cloud/faq/mixamo-faq.html). Lite uses original character and arm geometry, plus the project’s verified gameplay hitbox samples.

## Dependencies

- [Babylon.js core and loaders](https://github.com/BabylonJS/Babylon.js) use Apache License 2.0 and are used by the reference tools. The lockfile records their versions.
- [SDL2](https://github.com/libsdl-org/SDL) and [zlib](https://zlib.net/) retain their own licenses and are used by the desktop build.
- The ESP32-P4 firmware uses [ESP-IDF](https://github.com/espressif/esp-idf), FreeRTOS, and `esp_codec_dev`. The release package includes their license notices.
- The Guition panel initialization table is derived from the [Guition BSP](https://github.com/ultramcu/guition-jc4880p4-bsp). It retains the [BSP MIT license](firmware/GUITION-BSP-LICENSE).
