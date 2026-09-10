# Contributing

Changes should remain small, testable, and easy to roll back on hardware.

## Development workflow

1. Create a branch from `main`.
2. Make the firmware or documentation change.
3. Update `FW_VERSION` in `src/main.cpp` when preparing a release.
4. Run `pio run -e esp32-c3-super-mini`.
5. Test OFF, continuous, and burst modes on hardware when behavior changes.
6. Test calibration, settings persistence, sleep/wake, and OTA when those areas change.
7. Add an entry to `RELEASE_NOTES.md` before publishing firmware.
8. Open a pull request describing the change, test results, and known limitations.

Do not commit `.pio/` output or generated `.bin` files. Attach factory and OTA images to the corresponding GitHub release.

## Safety checks

- Keep GPIO0 high-impedance whenever firing is off.
- Preserve the 1 kΩ series-resistor requirement in documentation.
- Never assume every DualSense board revision uses the same voltages or polarity.
- Verify that USB and controller power are not connected together without safe isolation.
- Keep a known-good rollback image available during hardware testing.

See [`docs/BUILDING_AND_RELEASING.md`](docs/BUILDING_AND_RELEASING.md) for the complete release procedure.
