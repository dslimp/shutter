# AGENTS.md

## Profile

- Name: `shutter-local-dev`
- Repo: `/Users/user/Documents/shutter`
- Communication language: `ru`
- Communication style: `short-pragmatic`

## Project Scope

- Device family: `ESP8266`
- Main board for this project: `Wemos ESP-WROOM-02`
- Motor: `28BYJ-48` via `ULN2003`
- Power: `18650` with proper DC-DC conversion
- Current phase: bring-up and hardware validation (no deep sleep tuning yet)

## Firmware Targets

- Primary PlatformIO env: `wroom_02`
- Test env: `native`
- Do not switch to ESP32 targets in this repo.

## Connectivity

- Default Wi-Fi connect on boot:
  - SSID: `nh`
  - Password: `Fx110011`
- Fallback AP config portal:
  - SSID: `Shutter-Setup`
  - Password: `shutter123`
- In validation phase keep Wi-Fi awake (`WIFI_NONE_SLEEP`).

## API Contract

- Implemented endpoints:
  - `GET /api/state`
  - `POST /api/move`
  - `POST /api/calibrate`
  - `POST /api/settings`
  - `POST /api/wifi/reset`
  - `GET/POST /api/firmware/config`
  - `POST /api/firmware/update/latest`
  - `POST /api/firmware/update/release`
  - `POST /api/firmware/update/url`
- Do not reintroduce peristaltic pump endpoints (`/api/flow`, `/api/dosing`, etc.) in this repo.

## Timekeeping & Sun Schedule

- Time source: NTP via `configTime` (default servers: `pool.ntp.org`, `time.nist.gov`).
- Timezone is configurable via POSIX TZ string in settings (`timezone`), default `MSK-3`.
- Autonomous shutter schedule fields in `POST /api/settings`:
  - `sunScheduleEnabled` (bool)
  - `latitude`, `longitude` (float)
  - `sunriseOffsetMinutes`, `sunsetOffsetMinutes` (int, minutes)
  - `sunriseTargetPercent`, `sunsetTargetPercent` (0..100)
- Runtime status in `GET /api/state` includes:
  - `timeSynced`, `localTime`, `timezone`
  - `sunScheduleReady`, `sunriseTime`, `sunsetTime`
  - `sunriseDoneToday`, `sunsetDoneToday`
- Scheduler constraints:
  - runs only when calibrated (`calibrated=true`), schedule enabled, and time synchronized.
  - does not run while OTA job is pending/running.

## Calibration Model

- No reed/end-stop switches.
- Calibration is manual:
  - `set_top` -> logical 0%
  - `set_bottom` -> logical 100%
- Persist calibration/settings/current position in `EEPROM` (primary storage).
- Keep `/state.json` only as legacy migration source for old firmware.
- EEPROM migration policy (mandatory for every future firmware release):
  - Do not reorder or delete fields in existing persisted blob structs.
  - When schema changes, add explicit legacy struct + migrator (`applyPersistedBlobV<N>`).
  - In loader, try current schema first, then legacy schemas in descending order.
  - After successful legacy load, immediately rewrite EEPROM in current schema (one-time migration).
  - A release is not complete until migration path is verified on hardware from at least one previous schema.

## Validation Workflow

- Build firmware: `pio run`
- Build filesystem image: `pio run -t buildfs`
- Unit tests: `pio test -e native`
- Hardware regression suite (mandatory before release/OTA changes): `bash scripts/hw_regression_suite.sh 192.168.88.74`
- Upload firmware: `pio run -t upload`
- Upload filesystem: `pio run -t uploadfs`
- OTA from public URLs (if no serial): `POST /api/firmware/update/url` with both `firmwareUrl` and `filesystemUrl`
- Serial monitor: `pio device monitor -b 115200`

## Device Flashing Guide

- Full serial flashing (recommended for recovery/bootstrap):
  1) Build: `pio run -e wroom_02`
  2) Flash firmware: `pio run -e wroom_02 -t upload --upload-port /dev/cu.usbserial-0001`
  3) Flash filesystem: `pio run -e wroom_02 -t uploadfs --upload-port /dev/cu.usbserial-0001`
  4) Verify version: `curl -sS http://192.168.88.74/api/state`
- UART monitoring notes:
  - Boot ROM logs are `74880`, runtime logs are `115200`.
  - Visible "noise" right after reset is expected when monitor baud does not match the current boot/runtime phase.
- OTA bootstrap rule:
  - Safe OTA baseline is `v0.1.10`.
  - If a device is on legacy firmware with unstable GitHub OTA path, first move it to `0.1.10` via serial flash or `POST /api/firmware/update/url`, then continue regular GitHub OTA.

## Flash Port

- Default upload port: `/dev/cu.usbserial-0001`
- Use this port for upload and monitor unless user explicitly asks to change it.

## Guardrails

- Keep implementation focused on shutters, not pump logic.
- Avoid adding deep sleep logic in this phase unless user asks explicitly.
- Preserve existing UI style adapted from pump project.
- OTA reliability is a top-priority support requirement for this project.
  - Every OTA-related change must be validated on real hardware with both paths:
    1) GitHub OTA (`latest` and selected release),
    2) local URL OTA.
  - For release readiness, run at least 2-3 update cycles per path and verify post-update version/state.
  - If OTA fails, treat it as a release blocker and do not mark the task complete.
