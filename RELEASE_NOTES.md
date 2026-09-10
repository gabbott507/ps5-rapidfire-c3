# Project Rapid Fire release notes

This file is the source of truth for firmware changes. Every future firmware image should have an entry here before it is copied to `dist/` or published on the website.

## v0.4.8 — current release

- Includes the subsequent burst-control fixes after v0.4.6.
- Includes the current timer/ADC deep-sleep wake behavior.
- Retains continuous rapid fire, burst rapid fire, trigger calibration, saved settings, the Wi-Fi dashboard, OTA updates, and automatic sleep.
- Recommended starting settings: 8–10 SPS and 25–35 ms pulse width.
- Factory image: `dist/r2-rapidfire-c3-v0.4.8.bin`
- OTA image: `dist/r2-rapidfire-c3-v0.4.8-ota.bin`

## v0.4.7

- Image is retained in the archive, but detailed change notes were not recorded at the time of release.
- Do not use this entry as a replacement for v0.4.8 unless testing requires it.

## v0.4.6 — do not use

- Introduced an immediate-wake sleep regression because GPIO wake remained enabled.

## v0.4.5

- Image is retained in the archive, but detailed change notes were not recorded at the time of release.

## v0.4.4 — do not use

- Introduced firing regressions from aggressive release probing.

## v0.4.3 — stable rollback

- Retained as the known stable rollback image.
- Factory image: `dist/r2-rapidfire-c3-v0.4.3.bin`

## v0.1.0–v0.4.2

- Images are present in the archive, but detailed version-by-version notes were not recorded.
- v0.4.2 and later use the dual-OTA partition table and can receive later `-ota.bin` updates through the dashboard.

## Release checklist

Before publishing a new version:

1. Record what changed, what was tested, and any known limitations here.
2. Build the factory and OTA images and record the exact filenames.
3. Test a factory USB install and an OTA update when applicable.
4. Update the website download list, manifest, and release notes.
5. Mark any unsafe or superseded version clearly.
