# Install Fall of Duty Lite on the Guition JC4880P443C_I_W

Use the package with the Guition ESP32-P4 handheld and a wired Sony DualShock 4. This build uses the pre-v3 silicon configuration, 16 MB flash, an ST7701S panel, GT911 touch, ES8311 audio, and the board's SDMMC card slot.

1. Extract the zip. Use the firmware and SD pack it contains together. The compact schema 7 pack requires firmware with schema 7 support. From the extracted directory, verify the firmware and SD files with `shasum -a 256 -c SHA256SUMS` on macOS or `sha256sum -c SHA256SUMS` on Linux.
2. Copy the **contents** of `sd/` to the root of a FAT32 microSD card. Keep `fall-of-duty.fodp` and `campaign-voices.fodv` together at the card root for voiced campaign play. Insert the card.
3. Connect the board's ESP32-P4 programming USB port. Identify the new serial port by comparing the ports present with the board unplugged and plugged in. Set `FOD_PORT` to that exact device. Run the flash ID command before proceeding and confirm the expected chip, revision and capacity. Flashing replaces the bootloader, partition table and application on that board.
4. Connect the controller to the board's host port and connect a speaker. Select **Bot Match** with Cross or touch to play the displayed lineup. For a three-way match, open **Change Match**, set **Mode** to **Free-for-all**, choose an arena, then select **Start Match**. First to ten wins. If the board restarts to change arenas, play begins with the selected settings once the controller is ready.
5. For Operation Briar, choose **Campaign → Solo Campaign**. Finishing either of the first two missions saves the next chapter in the board’s settings storage. Choose **Campaign → Continue** after a power cycle to pick up there.

With the ESP-IDF v5.5.3 Python environment active, run from the extracted package directory.

```sh
export FOD_PORT=/dev/cu.usbserial-REPLACE_WITH_BOARD
python -m esptool --chip esp32p4 --port "$FOD_PORT" flash_id
cd firmware
python -m esptool --chip esp32p4 --port "$FOD_PORT" --before default_reset --after hard_reset write_flash @flash_args
```

| DualShock 4 | Action |
| --- | --- |
| Left stick, right stick | Move, look |
| L3 | Sprint |
| L2, R2 | Aim, fire |
| Cross, Circle | Jump or confirm, tap crouch or hold prone |
| Square, Triangle | Reload, swap weapon |
| D-pad up | Use banked streak or put away the laptop |
| D-pad and Options | Navigate, pause or resume |
| Right stick and R2 with laptop out | Move the cursor and mark the airstrike |

A missing or invalid pack opens the recovery screen. Insert the matching card, then tap Retry or press Cross. If the flash tool cannot connect, hold P4 BOOT while resetting, release it when the serial loader enumerates, and repeat the flash command with that port. On controller disconnect, reconnect, release held buttons and resume explicitly.

Both **400 × 225** and **320 × 180** are selectable in the menu. This package is cross-built for the Guition profile. Panel, audio, input, and full-duel verification on the target are pending.
