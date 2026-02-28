# AGENTS.md

## Profile

- Name: `shutter-local-dev`
- Repo: `/Users/user/Documents/shutter`
- Communication language: `ru`
- Communication style: `short-pragmatic`

## Project Scope

- Device: `Wemos D1 mini / ESP8266 (ESP-WROOM-02)`
- Motor: `28BYJ-48` via `ULN2003`
- Power: `18650` with proper DC-DC conversion
- Current phase: bring-up and hardware validation (no deep sleep tuning yet)

## Firmware Targets

- Primary PlatformIO env: `d1_mini`
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
- Do not reintroduce peristaltic pump endpoints (`/api/flow`, `/api/dosing`, etc.) in this repo.

## Calibration Model

- No reed/end-stop switches.
- Calibration is manual:
  - `set_top` -> logical 0%
  - `set_bottom` -> logical 100%
- Persist calibration and current position in `LittleFS` (`/state.json`).

## Validation Workflow

- Build firmware: `pio run`
- Build filesystem image: `pio run -t buildfs`
- Unit tests: `pio test -e native`
- Upload firmware: `pio run -t upload`
- Upload filesystem: `pio run -t uploadfs`
- Serial monitor: `pio device monitor -b 115200`

## Flash Port

- Default upload port: `/dev/cu.usbserial-0001`
- Use this port for upload and monitor unless user explicitly asks to change it.

## Guardrails

- Keep implementation focused on shutters, not pump logic.
- Avoid adding deep sleep logic in this phase unless user asks explicitly.
- Preserve existing UI style adapted from pump project.
