# countdown

**Countdown** · v1.0.0

Digital countdown with a progress bar and end-beep.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#tool` `#time` `#audio` `#offline`

Pick a duration up to 3 hours and watch it count down. Big mm:ss digits in the middle, a horizontal progress bar drains from green to red as time runs out. Beeps at zero.

## Controls
- **BOOT** — start / pause
- **PWR** — adjust duration in 1-minute steps
- long-press **BOOT** — reconfigure duration / end-beep

## `setup.txt` keys (all optional)
- `COUNTDOWN_MINUTES` — initial duration in minutes (1–180)
- `COUNTDOWN_BEEP` — `1` for end-beep, `0` for silent

`setup.txt` only seeds the first boot; in-app long-press **BOOT** to change later.

## Editing `setup.txt`
The device reads `/setup/setup.txt` from the SD card on boot. [Download a working sample](https://sosbxffigpteqilpgxwn.supabase.co/storage/v1/object/public/app-assets/setup/setup.txt) — covers every app — and edit the keys you need.

Don't want to eject the card? Use the [**USB Stick**](/apps/usb-stick) app (mounts the SD card as a USB drive over USB-C) or the [**Filehub**](/apps/filehub) app (edit over WiFi).

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - GFX Library for Arduino (moononournation)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/countdown_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/countdown_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/countdown).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/countdown
