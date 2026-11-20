# Fall of Duty Lite firmware for the Guition JC4880P443C_I_W

The firmware uses ESP-IDF v5.5.3 at `2c211b236707889e8400c4dc5644dd5c4ee071e0`, the IDF-selected RISC-V GCC 14.2.0, and `esp_codec_dev` 1.3.4. The profile targets ESP32-P4 revisions below v3 with an image range of v0.0–v1.99 and a 360 MHz CPU setting. Its other build settings are 16 MB flash, 200 MHz PSRAM and a requested 34 MHz DPI pixel clock. The boot log reports detected silicon revision, CPU clock, flash and PSRAM capacities, SDK version, and configured PSRAM speed. Clock and capacity readings from a specific board require a boot. [ESP32-P4 revision guidance](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/system.html#does-esp-idf-v5-5-3-support-esp32-p4-v1-3)

The ST7701S command table and timings are from the Guition BSP at `324970bade0d1f4e52880fe8016580368bc1e06e`. The third-party table retains its MIT notice. Touch polls the GT911 on the shared codec I²C bus and applies the inverse display rotation. The TF card uses SDMMC and LDO4. The wired controller uses the P4 high-speed USB host port. The audio task feeds 5 ms PCM blocks to I²S independently of rendering. [Board pinout and schematic](https://github.com/ultramcu/guition-jc4880p443c-i-w) · [BSP license](firmware/GUITION-BSP-LICENSE) · [ES8311 component](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.3.4)

## Build and install

With Git, Python, CMake and Ninja on PATH, run these commands from the repository root.

```sh
git clone --recursive --branch v5.5.3 https://github.com/espressif/esp-idf.git "$HOME/esp-idf-v5.5.3"
"$HOME/esp-idf-v5.5.3/install.sh" esp32p4
. "$HOME/esp-idf-v5.5.3/export.sh"
cd firmware
idf.py -B build-lite -D SDKCONFIG=build-lite/sdkconfig build
```

Copy `fall-of-duty.fodp` to the root of a FAT32 microSD card and insert it before boot. Connect the flash/programming USB port, identify the newly appeared serial port, and set `FOD_PORT` to that exact device name. Use the chip and flash ID query to verify the Guition P4 before writing it. Connect the speaker and wired DualShock 4 on the host port to play.

```sh
export FOD_PORT=/dev/cu.usbserial-REPLACE_WITH_BOARD
python -m esptool --chip esp32p4 --port "$FOD_PORT" flash_id
idf.py -B build-lite -p "$FOD_PORT" flash monitor
```

The build's `flash_args` records the bootloader, partition table and application offsets for installing prebuilt binaries. Monitor at 115200 baud. Application logs are routed to UART0 on GPIO37 TX and GPIO38 RX. If the port selected for programming carries USB Serial/JTAG, use a separate 3.3 V logic-level UART adapter on these pins to read application logs. If auto-reset fails, hold the board's P4 BOOT button while resetting or powering the board, release BOOT when the serial loader enumerates, and rerun the flash command.

A missing, malformed or incompatible pack shows the recovery screen. Insert the correct FAT32 card, then tap Retry or press Cross. On SD or pack validation failure the filesystem is unmounted before the retry.

## Controls

| DualShock 4 | Game |
| --- | --- |
| Left and right sticks | Move and look |
| L3 | Sprint |
| L2 and R2 | Aim and fire |
| Cross | Jump or confirm |
| Circle | Tap crouch, hold prone, menu back |
| Square | Reload |
| Triangle | Swap weapon |
| D-pad up | Deploy the banked streak or close laptop |
| D-pad | Navigate menus |
| Options | Pause or resume |
| Right stick and R2 on airstrike laptop | Move the cursor and mark the location |

The USB driver accepts Sony VID `054c` and PID `05c4` or `09cc` with input report `01` on HID interface 0. It logs the actual IDs, endpoint and packet size when the controller is connected. On a disconnect the match pauses and waits for an explicit resume after reconnection. Triggers and buttons held when the controller returns require release. Touch selects menu settings and can pause the duel from the HUD.

## Display and input bring-up

The diagnostic uses the same panel, DMA ownership, controller, audio and SD paths as the game. Use a separate output directory for every configuration because ESP-IDF persists Kconfig selections in the generated `sdkconfig`.

```sh
idf.py -B build-diag -D SDKCONFIG=build-diag/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.diagnostic' build
idf.py -B build-diag -p "$FOD_PORT" flash monitor
```

With the display running, verify that the sequence and framebuffer index follow the monitor log. Check the moving number against alternating backgrounds and verify all four numbered corners, including the 15-pixel margins. Touch each corner and record the coordinate and visible marker location. Triangle switches the diagnostic world between 400×225 and 320×180. Change the mode within the initial 10-second warm-up or use a fresh boot for each capture. Exercise controller inputs, a mounted card and the periodic speaker tone concurrently.

For the static scanout baseline set `FOD_DIAGNOSTIC_SCANOUT_ONLY` in that build's menuconfig. This sends a single numbered image and measures the DMA cadence and underrun counter without ongoing PPA writes. Return to the moving diagnostic to add PPA and margin reuse. DMA start timestamps approximate scanout boundaries. Confirm the relationship to actual distinct visible frames with a camera or photodiode during qualification.

The driver modification is generated in each build directory from the pinned stock DPI source. It selects a new framebuffer every second DMA restart and reports the index programmed into the DMA link. The producer keeps ownership of the selected and pending buffers separate. The PPA writes into a free native buffer, then the CPU draws the HUD and the submission path writes the cache back before publishing its index to the ISR. [Pinned stock DPI source](https://github.com/espressif/esp-idf/blob/v5.5.3/components/esp_lcd/dsi/esp_lcd_panel_dpi.c) · [Board patch](firmware/cmake/dpi_scanout.cmake)

When a late submission is waiting for scanout, the producer waits for the next selection before beginning another frame. The underrun interrupt increments a counter and returns. The benchmark task reports the count when capture ends, so UART logging stays outside the measured interrupt path.

## Replay and measurement

Copy the desired `.fodtrace` files to the SD root with the pack. To run a looped trace at 400×225, create a Kconfig overlay and flash that build. Use `apache` for the other scene and set `CONFIG_FOD_REPLAY_COMPACT=y` for 320×180. Use a fresh build directory and a new overlay for each combination. The default renderer includes baked daylight, the new sky, improved depth precision and texture mipmaps. Add `CONFIG_FOD_CLASSIC_VISUALS=y` to an overlay for the prior render path and the same two selectable resolutions.

```sh
printf '%s\n' 'CONFIG_FOD_REPLAY_SCENE="long-lane"' > /tmp/fod-lane-400.defaults
idf.py -B build-lane-400 -D SDKCONFIG=build-lane-400/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;/tmp/fod-lane-400.defaults' build
idf.py -B build-lane-400 -p "$FOD_PORT" flash monitor
```

After the 10-second warm-up, the firmware records a predecessor plus 3,000 scheduled presentations. The serial summary counts repeated sequences as misses and reports consecutive misses, the maximum gap and interval percentiles. It also reports display and I²S underruns. The two CSV files are written to SD after capture finishes. The raw data distinguishes world cost, raster cost, PPA, HUD and submission. Preserve the files under a name containing scene, mode and pack revision before starting another capture.

Add `CONFIG_FOD_REPLAY_FRAME_ONLY=y` to the overlay to measure total frame work with only a timer before rendering and after submission. This mode omits the renderer timers, the PPA and HUD timestamps, and the request-slot lookup. Use it for paired measurements of the live render path. For detailed captures, the report derives the same total by adding the world, PPA, HUD and submission intervals per frame.

```sh
python3 bench_report.py /path/to/copied/slots.csv /path/to/copied/stages.csv
```

Add `--audio-underruns N` with the captured counter from the serial summary after confirming `audio_ready=1`. To test latency, choose an unused output pad confirmed against the board schematic and set `CONFIG_FOD_LATENCY_GPIO` to that GPIO in a separate instrumented build. Put the photodiode over the center marker and collect report and optical edge pairs on the same timebase. Vary the R2 press timing through the panel period, then pass a CSV with `report_us,photon_us` columns using `--latency`. Compare p95 with two measured presentation periods. Preserve the raw optical trace and note the screen location and controller IDs.

For parity replay, add `CONFIG_FOD_REPLAY_JOURNAL=y` with the desired scene in a separate build. It writes `native.jsonl` from the same portable simulation and shared trace logic and logs missing and unused random draws. Retrieve it after the write completes. Keep journal writes separate from timed captures.

## Qualification status

No Guition appeared in the local USB registry or serial inventory. No hardware result is recorded. The remaining checks are panel orientation and optical frame ownership, all four touch corners, USB report and reconnect behavior, audio and SD recovery under scanout, four scene and resolution performance captures, optical latency, and full packaged first-to-ten play on both passing modes. Record measured PSRAM and internal memory at boot, pack load, peak action and after ten completed rematches. Record the time from pack load to `renderer-ready`, which includes the daylight bake. The default mode is 400×225 pending these measurements.
