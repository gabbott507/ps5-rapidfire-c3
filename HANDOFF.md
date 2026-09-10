# Project handoff

## Current release

Current development release: **v0.4.8**

- Full USB/factory image: `dist/r2-rapidfire-c3-v0.4.8.bin`
- Wireless application image: `dist/r2-rapidfire-c3-v0.4.8-ota.bin`
- Wi-Fi SSID: `R2-RapidFire`
- Wi-Fi password: `12345678`
- Dashboard: `http://192.168.4.1`

The dashboard displays the running firmware version. Version 0.4.2 introduced the dual-OTA partition table; devices on v0.4.2 or newer can install subsequent `-ota.bin` files wirelessly.

## Hardware

- ESP32-C3 Super Mini
- DualSense R2 signal through a 1 kΩ series resistor to GPIO0
- DualSense ground connected to ESP ground
- ESP powered from the controller battery in the tested installation

Do not connect controller battery power and USB power simultaneously unless safe power-path isolation has been verified. The R2 signal is analog and uses decreasing polarity on the tested controller:

- Released ADC: approximately 922–934
- Fully pressed ADC: approximately 267–270

## Build environment

This project must use the Arduino-ESP32 3.x-compatible PioArduino platform configured in `platformio.ini`. The older standard PlatformIO Arduino-ESP32 2.0.17 stack produced a three-second reset loop on the tested ESP32-C3 revision.

Build with PlatformIO:

```sh
pio run -e esp32-c3-super-mini
```

Generated images:

- `.pio/build/esp32-c3-super-mini/firmware.factory.bin` — full USB image at offset `0x0`
- `.pio/build/esp32-c3-super-mini/firmware.bin` — application-only OTA image

The partition layout is `default.csv`, providing `ota_0` and `ota_1`, each 1280 KiB.

## Features

- Continuous rapid fire, 1–40 SPS
- Burst rapid fire, 1–12 SPS and 1–10 shots
- Separate continuous and burst rates
- Adjustable pulse width, press threshold, debounce, and hysteresis
- Released/pressed trigger calibration
- Wi-Fi dashboard and captive DNS
- NVS-persisted settings
- Dashboard OTA upload with image validation
- Firmware version in dashboard and status API
- Automatic sleep and Sleep Now
- Timer/ADC wake polling every 250 ms; two pressed samples are required
- RESET always wakes immediately

## Sleep behavior

Wi-Fi is explicitly disabled before deep sleep. GPIO wake was removed because the analog R2 level immediately retriggered the ESP. The current implementation wakes by timer every 250 ms, checks R2 without starting Wi-Fi, and returns to sleep unless two consecutive valid pressed readings are detected.

The controller must be on for its R2 sensor to produce valid ADC readings. When the controller is off, GPIO0 reads approximately zero and pulling R2 cannot wake the ESP. Expected wake sequence:

1. Turn on the DualSense.
2. Hold R2 for roughly half a second.
3. Wait for `R2-RapidFire` to reappear.

The ESP32-C3 board's hardwired power LED and regulator still consume power during sleep. Removing the LED resistor or using a controller-switched supply would improve battery life further.

## Firing behavior and limitations

The one-wire circuit cannot drive the controller-facing R2 signal and independently read the physical trigger at the same instant. Firmware alternates GPIO0 between output and high-impedance ADC input. This imposes unavoidable release-detection tradeoffs without a second sensing wire or external analog hardware.

Burst mode treats the initial physical R2 edge as shot one and generates only the remaining transitions. High SPS values can still cause a game to ignore pulses above a weapon's allowed firing cadence. Start at 8 burst SPS and reduce it when the in-game shot count is low.

Versions v0.4.4 and v0.4.6 should not be used:

- v0.4.4 introduced firing regressions from aggressive release probing.
- v0.4.6 could immediately wake from sleep because GPIO wake remained enabled.

v0.4.3 is retained as a stable rollback image. v0.4.8 contains the subsequent burst-control and sleep fixes.

## Important files

- `src/main.cpp` — firmware and state machine
- `include/web_ui.h` — embedded dashboard
- `platformio.ini` — board, framework, toolchain, and partition configuration
- `README.md` — user setup instructions
- `HANDOFF.md` — continuation notes

## Suggested next work

1. Measure real controller battery drain awake versus asleep.
2. Test v0.4.8 burst counts across several weapons at 6, 8, 10, and 12 SPS.
3. Confirm continuous mode does not add a trailing shot on release.
4. Consider a second R2 sensing wire or analog switch for truly independent trigger sensing and output.
5. Potential future L2 hair-trigger support requires identifying and measuring the L2 signal pad before coding.
