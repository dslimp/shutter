# Changelog

All notable changes to this project are documented in this file.

## [0.1.9] - 2026-02-28

- OTA reliability hardening for ESP8266:
  - added firmware/filesystem OTA retries (`3` attempts) with short backoff,
  - added detailed serial diagnostics per OTA attempt (RSSI, heap, URL, result/error),
  - removed manual watchdog disable/enable around OTA path to avoid watchdog-reset instability.
- OTA release selector in UI now uses GitHub Releases API (`/releases`) instead of tags (`/tags`), and skips drafts.

## [0.1.8] - 2026-02-28

- Moved persistent controller state to EEPROM (primary), with legacy `/state.json` migration on first boot.
- Fixed calibration persistence: `set_top`, `set_bottom`, and `reset` now force immediate save.
- Added hardware regression suite for real device validation:
  - settings persistence (`reverseDirection`, speeds, hold time),
  - calibration persistence across reboot and OTA,
  - OTA repo default fallback and latest URL check.
- Removed stress-test release padding from firmware/filesystem build configuration.

## [0.1.6] - 2026-02-28

- OTA stress-test build: increased firmware size (`OTA_FW_PAD_BYTES=65536`) and filesystem image (extra `data/ota_pad.bin`).

## [0.1.5] - 2026-02-28

- OTA pipeline switched from manual `Update.writeStream` to `ESP8266httpUpdate` for better stability.
- Added AGENTS guardrail that OTA reliability is a release blocker and requires repeated hardware verification.

## [0.1.4] - 2026-02-28

- Added configurable top overdrive for `open` command:
  - `topOverdriveEnabled` (default `true`)
  - `topOverdrivePercent` (default `10`)
- Open motion can run beyond logical `0%`, then internal position is re-anchored to `0` after stop.
- Added calibration tab `Стоп` button.
- OTA UI now supports selecting a specific GitHub tag and updating to that selected release.

## [0.1.3] - 2026-02-28

- OTA HTTP client now follows redirects, fixing GitHub `latest/download` update path (`302`).
- Bumped firmware-reported version to `0.1.3-esp8266`.

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
