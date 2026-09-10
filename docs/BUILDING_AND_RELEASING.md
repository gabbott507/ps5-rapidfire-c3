# Building and releasing

## Prerequisites

- Python and PlatformIO Core, or the PlatformIO IDE extension
- A data-capable USB cable for factory flashing
- An ESP32-C3 Super Mini connected with the documented wiring

The project deliberately pins the platform source in `platformio.ini` to an Arduino-ESP32 3.x-compatible PioArduino release. The older standard PlatformIO Arduino-ESP32 2.0.17 stack caused a reset loop on tested hardware.

## Build

From the repository root:

```sh
pio run -e esp32-c3-super-mini
```

Important output files:

- `.pio/build/esp32-c3-super-mini/firmware.factory.bin` — merged factory/USB image, flashed at offset `0x0`
- `.pio/build/esp32-c3-super-mini/firmware.bin` — application-only OTA image

The `default.csv` partition layout provides two OTA slots. Devices must first receive a compatible factory image; v0.4.2 and newer can accept later OTA application images.

## Optional diagnostic build

```sh
pio run -e wifi-diagnostic
```

This build starts the `R2-Diagnostic` access point and is intended only for isolating Wi-Fi startup issues.

## Hardware test checklist

- Confirm OFF mode leaves normal R2 behavior unchanged.
- Capture released and fully pressed values and apply calibration.
- Test continuous mode at representative SPS and pulse widths.
- Test exact burst counts at 6, 8, 10, and 12 SPS.
- Confirm releasing R2 does not add an unwanted trailing shot.
- Confirm saved settings survive a restart.
- Confirm automatic sleep, **Sleep now**, and trigger wake behavior.
- Perform both a factory USB install and an OTA update.
- Measure supply voltage and current if wiring or power behavior changed.

## Release procedure

1. Update `FW_VERSION` in `src/main.cpp`.
2. Add the release notes and test results to `RELEASE_NOTES.md`.
3. Build the `esp32-c3-super-mini` environment.
4. Rename `firmware.factory.bin` to `r2-rapidfire-c3-vX.Y.Z.bin`.
5. Rename `firmware.bin` to `r2-rapidfire-c3-vX.Y.Z-ota.bin`.
6. Test both artifacts on hardware.
7. Commit the source and documentation, then tag the commit `vX.Y.Z`.
8. Create a GitHub release and attach both binaries.

Also keep the browser flasher (<https://projectrapidfire.pdulab.org>) in sync:
place `r2-rapidfire-c3-vX.Y.Z.bin` in the site's `firmware/` directory and update
the filename and version in the site's `flasher.js` and `index.html`.

Never upload the factory image through the dashboard. Dashboard updates must use the file ending in `-ota.bin`.
