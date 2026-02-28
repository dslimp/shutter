# Changelog

All notable changes to this project are documented in this file.

## [0.1.2] - 2026-02-28

- Bumped firmware-reported version to `0.1.2-esp8266` in `/api/state`.
- Keeps all `0.1.1` changes (WROOM-02 target, A0 in state/UI, no-limits calibration jog).

## [0.1.1] - 2026-02-28

- Board target fixed to `Wemos ESP-WROOM-02` (`esp_wroom_02`) with GPIO-based pin mapping.
- LittleFS partition layout fixed for state persistence (`eagle.flash.2m128.ld`).
- OTA config normalization improved: empty repo/assets fallback to `dslimp/shutter`, strict repo format validation.
- Added `A0` raw ADC value to `/api/state` and top-panel display in UI.
- Calibration jog now has a dedicated no-limits path (`POST /api/calibrate` with `{"action":"jog","steps":...}`).
- Hardware smoke test extended with OTA config fallback checks.

## [0.1.0] - 2026-02-28

- Initial ESP8266 shutter controller release.
- Web UI for movement, calibration, and settings.
- Position and calibration persistence in LittleFS.
- Default Wi-Fi connect (`nh`) with AP fallback (`Shutter-Setup`).
- Hardware smoke test script for settings persistence on test device.
